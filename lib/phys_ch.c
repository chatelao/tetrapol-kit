#include "tetrapol.h"
#include "multiblock.h"
#include "tpdu.h"
#include "phys_ch.h"
#include "misc.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define DEBUG

// max error rate for 2 frame synchronization sequences
#define MAX_FRAME_SYNC_ERR 1

#define FRAME_HDR_LEN (8)
#define FRAME_DATA_LEN (152)
#define FRAME_LEN (FRAME_HDR_LEN + FRAME_DATA_LEN)

typedef struct {
    int frame_no;
    uint8_t data[FRAME_DATA_LEN];
} frame_t;

struct _phys_ch_t {
    int last_sync_err;  ///< errors in last frame synchronization sequence
    int total_sync_err; ///< cumulative error in framing
    bool has_frame_sync;
    int frame_no;
    int data_len;
    int scr;            ///< SCR, scrambling constant
    int scr_confidence; ///< required confidence for SCR detection
    int scr_stat[128];  ///< statistics for SCR detection
    uint8_t data[10*FRAME_LEN];
};

enum {
    FRAME_TYPE_AUDIO = 0,
    FRAME_TYPE_DATA = 1,
};

/**
  PAS 0001-2 6.1.5.1
  PAS 0001-2 6.2.5.1
  PAS 0001-2 6.3.4.1

  Scrambling sequence was generated by this python3 script

  s = [1, 1, 1, 1, 1, 1, 1]
  for k in range(len(s), 127):
    s.append(s[k-1] ^ s[k-7])
  for i in range(len(s)):
    print(s[i], end=", ")
    if i % 8 == 7:
      print()
  */
static uint8_t scramb_table[127] = {
    1, 1, 1, 1, 1, 1, 1, 0,
    1, 0, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 1, 1, 0, 1,
    1, 1, 0, 1, 0, 0, 1, 0,
    1, 1, 0, 0, 0, 1, 1, 0,
    1, 1, 1, 1, 0, 1, 1, 0,
    1, 0, 1, 1, 0, 1, 1, 0,
    0, 1, 0, 0, 1, 0, 0, 0,
    1, 1, 1, 0, 0, 0, 0, 1,
    0, 1, 1, 1, 1, 1, 0, 0,
    1, 0, 1, 0, 1, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 0,
    1, 0, 0, 1, 1, 1, 1, 0,
    0, 0, 1, 0, 1, 0, 0, 0,
    0, 1, 1, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0,
};

static int process_frame(phys_ch_t *phys_ch, frame_t *frame);

phys_ch_t *tetrapol_phys_ch_create(void)
{
    phys_ch_t *phys_ch = malloc(sizeof(phys_ch_t));
    if (phys_ch == NULL) {
        return NULL;
    }
    memset(phys_ch, 0, sizeof(phys_ch_t));

    phys_ch->frame_no = FRAME_NO_UNKNOWN;
    phys_ch->scr = PHYS_CH_SCR_DETECT;
    phys_ch->scr_confidence = 50;

    return phys_ch;
}

void tetrapol_phys_ch_destroy(phys_ch_t *phys_ch)
{
    free(phys_ch);
}

int tetrapol_phys_ch_get_scr(phys_ch_t *phys_ch)
{
    return phys_ch->scr;
}

void tetrapol_phys_ch_set_scr(phys_ch_t *phys_ch, int scr)
{
    phys_ch->scr = scr;
    memset(&phys_ch->scr_stat, 0, sizeof(phys_ch->scr_stat));
}

int tetrapol_phys_ch_get_scr_confidence(phys_ch_t *phys_ch)
{
    return phys_ch->scr_confidence;
}

void tetrapol_phys_ch_set_scr_confidence(
        phys_ch_t *phys_ch, int scr_confidence)
{
    phys_ch->scr_confidence = scr_confidence;
}

static uint8_t differential_dec(uint8_t *data, int size, uint8_t last_bit)
{
    while (size--) {
        last_bit = *data = *data ^ last_bit;
        ++data;
    }
    return last_bit;
}

int tetrapol_recv2(phys_ch_t *phys_ch, uint8_t *buf, int len)
{
    const int space = sizeof(phys_ch->data) - phys_ch->data_len;
    len = (len > space) ? space : len;

    memcpy(phys_ch->data + phys_ch->data_len, buf, len);
    phys_ch->data_len += len;

    return len;
}

// compare bite stream to differentialy encoded synchronization sequence
static int cmp_frame_sync(const uint8_t *data)
{
    const uint8_t frame_dsync[] = { 1, 0, 1, 0, 0, 1, 1, };
    int sync_err = 0;
    for(int i = 0; i < sizeof(frame_dsync); ++i) {
        if (frame_dsync[i] != data[i + 1]) {
            ++sync_err;
        }
    }
    return sync_err;
}

/**
  Find 2 consecutive frame synchronization sequences.

  Using raw stream (before differential decoding) simplyfies search
  because only signal polarity must be considered,
  there is lot of troubles with error handlig after differential decoding.
  */
static int find_frame_sync(phys_ch_t *phys_ch)
{
    int offs = 0;
    int sync_err = MAX_FRAME_SYNC_ERR + 1;
    while (offs + FRAME_LEN + FRAME_HDR_LEN < phys_ch->data_len) {
        const uint8_t *data = phys_ch->data + offs;
        sync_err = cmp_frame_sync(data) +
            cmp_frame_sync(data + FRAME_LEN);
        if (sync_err <= MAX_FRAME_SYNC_ERR) {
            break;
        }

        ++offs;
    }

    phys_ch->data_len -= offs;
    memmove(phys_ch->data, phys_ch->data + offs, phys_ch->data_len);

    if (sync_err <= MAX_FRAME_SYNC_ERR) {
        phys_ch->last_sync_err = 0;
        phys_ch->total_sync_err = 0;
        return 1;
    }

    return 0;
}

/// return number of acquired frames (0 or 1) or -1 on error
static int get_frame(phys_ch_t *phys_ch, frame_t *frame)
{
    if (phys_ch->data_len < FRAME_LEN) {
        return 0;
    }
    const int sync_err = cmp_frame_sync(phys_ch->data);
    if (sync_err + phys_ch->last_sync_err > MAX_FRAME_SYNC_ERR) {
        phys_ch->total_sync_err = 1 + 2 * phys_ch->total_sync_err;
        if (phys_ch->total_sync_err >= FRAME_LEN) {
            return -1;
        }
    } else {
        phys_ch->total_sync_err = 0;
    }

    phys_ch->last_sync_err = sync_err;
    memcpy(frame->data, phys_ch->data + FRAME_HDR_LEN, FRAME_DATA_LEN);
    differential_dec(frame->data, FRAME_DATA_LEN, 0);
    phys_ch->data_len -= FRAME_LEN;
    memmove(phys_ch->data, phys_ch->data + FRAME_LEN, phys_ch->data_len);

    frame->frame_no = phys_ch->frame_no;

    return 1;
}

int tetrapol_phys_ch_process(phys_ch_t *phys_ch)
{
    if (!phys_ch->has_frame_sync) {
        phys_ch->has_frame_sync = find_frame_sync(phys_ch);
        if (!phys_ch->has_frame_sync) {
            return 0;
        }
        fprintf(stderr, "Frame sync found\n");
        phys_ch->frame_no = FRAME_NO_UNKNOWN;
        multiblock_reset();
        segmentation_reset();
    }

    int r = 1;
    frame_t frame;
    while ((r = get_frame(phys_ch, &frame)) > 0) {
        process_frame(phys_ch, &frame);
        if (frame.frame_no != FRAME_NO_UNKNOWN) {
            phys_ch->frame_no = (frame.frame_no + 1) % 200;
        }
    }

    if (r == 0) {
        return 0;
    }

    fprintf(stderr, "Frame sync lost\n");
    phys_ch->has_frame_sync = false;

    return 0;
}

// http://ghsi.de/CRC/index.php?Polynom=10010
static void mk_crc5(uint8_t *res, const uint8_t *input, int input_len)
{
    uint8_t inv;
    memset(res, 0, 5);

    for (int i = 0; i < input_len; ++i)
    {
        inv = input[i] ^ res[0];         // XOR required?

        res[0] = res[1];
        res[1] = res[2];
        res[2] = res[3] ^ inv;
        res[3] = res[4];
        res[4] = inv;
    }
}

static int frame_data_check_crc(const data_frame_t *df)
{
    uint8_t crc[5];

    mk_crc5(crc, df->data, 69);
    return !memcmp(df->data + 69, crc, 5);
}

/**
  PAS 0001-2 6.1.2
  PAS 0001-2 6.2.2
*/
static int channel_decoder(uint8_t *res, uint8_t *err, const uint8_t *in, int res_len)
{
#ifdef GET_IN_
#error "Collision in definition of macro GET_IN_!"
#endif
#define GET_IN_(x, y) in[((x) + (y)) % (2*res_len)]

    int errs = 0;
    for (int i = 0; i < res_len; ++i) {
        res[i] = GET_IN_(2*i, 2) ^ GET_IN_(2*i, 3);
        err[i] = GET_IN_(2*i, 5) ^ GET_IN_(2*i, 6) ^ GET_IN_(2*i, 7);

        // we have 2 solutions, if match set to 0, 1 othervise
        err[i] ^= res[i];
        errs += err[i];
    }
#undef GET_IN_

    return errs;
}

static int frame_decode_data(const frame_t *f, data_frame_t *df)
{
    // decode first 52 bites of frame
    int errs = channel_decoder(df->data, df->err, f->data, 26);
    // TODO: check frame type (AUDIO / DATA)
    // decode remaining part of frame
    errs += channel_decoder(df->data + 26, df->err + 26, f->data + 2*26, 50);

    df->frame_no = f->frame_no;

    return errs;
}

// PAS 0001-2 6.1.4.1
static const int interleave_voice_UHF[] = {
    1, 77, 38, 114, 20, 96, 59, 135,
    3, 79, 41, 117, 23, 99, 62, 138,
    5, 81, 44, 120, 26, 102, 65, 141,
    8, 84, 47, 123, 29, 105, 68, 144,
    11, 87, 50, 126, 32, 108, 71, 147,
    14, 90, 53, 129, 35, 111, 74, 150,
    17, 93, 56, 132, 37, 113, 73, 4,
    0, 76, 40, 119, 19, 95, 58, 137,
    151, 80, 42, 115, 24, 100, 60, 133,
    12, 88, 48, 121, 30, 106, 66, 139,
    18, 91, 51, 124, 28, 104, 67, 146,
    10, 89, 52, 131, 34, 110, 70, 149,
    13, 97, 57, 130, 36, 112, 75, 148,
    6, 82, 39, 116, 16, 92, 55, 134,
    2, 78, 43, 122, 22, 98, 61, 140,
    9, 85, 45, 118, 27, 103, 63, 136,
    15, 83, 46, 125, 25, 101, 64, 143,
    7, 86, 49, 128, 31, 107, 69, 142,
    21, 94, 54, 127, 33, 109, 72, 145,
};

// PAS 0001-2 6.2.4.1
static const int interleave_data_UHF[] = {
    1, 77, 38, 114, 20, 96, 59, 135,
    3, 79, 41, 117, 23, 99, 62, 138,
    5, 81, 44, 120, 26, 102, 65, 141,
    8, 84, 47, 123, 29, 105, 68, 144,
    11, 87, 50, 126, 32, 108, 71, 147,
    14, 90, 53, 129, 35, 111, 74, 150,
    17, 93, 56, 132, 37, 112, 76, 148,
    2, 88, 40, 115, 19, 97, 58, 133,
    4, 75, 43, 118, 22, 100, 61, 136,
    7, 85, 46, 121, 25, 103, 64, 139,
    10, 82, 49, 124, 28, 106, 67, 142,
    13, 91, 52, 127, 31, 109, 73, 145,
    16, 94, 55, 130, 34, 113, 70, 151,
    0, 80, 39, 116, 21, 95, 57, 134,
    6, 78, 42, 119, 24, 98, 60, 137,
    9, 83, 45, 122, 27, 101, 63, 140,
    12, 86, 48, 125, 30, 104, 66, 143,
    15, 89, 51, 128, 33, 107, 69, 146,
    18, 92, 54, 131, 36, 110, 72, 149,
};

static void frame_deinterleave(frame_t *f)
{
    uint8_t tmp[FRAME_DATA_LEN];
    memcpy(tmp, f->data, FRAME_DATA_LEN);

    for (int j = 0; j < FRAME_DATA_LEN; ++j) {
        f->data[j] = tmp[interleave_data_UHF[j]];
    }
}


/**
  PAS 0001-2 6.1.4.2
  PAS 0001-2 6.2.4.2

  Audio and data frame differencial precoding index table was generated by the
  following python 3 scipt.

  pre_cod = ( 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40,
             43, 46, 49, 52, 55, 58, 61, 64, 67, 70, 73, 76,
             83, 86, 89, 92, 95, 98, 101, 104, 107, 110, 113, 116,
            119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149 )
  for i in range(152):
      print(1+ (i in pre_cod), end=", ")
      if i % 8 == 7:
          print()
*/
static const int diff_precod_UHF[] = {
    1, 1, 1, 1, 1, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 1,
    1, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
};

static void frame_diff_dec(frame_t *f)
{
    for (int j = FRAME_DATA_LEN - 1; j > 0; --j) {
        f->data[j] ^= f->data[j - diff_precod_UHF[j]];
    }
}

static void frame_descramble(frame_t *f, int scr)
{
    if (scr == 0) {
        return;
    }

    for(int k = 0 ; k < FRAME_DATA_LEN; k++) {
        f->data[k] ^= scramb_table[(k + scr) % 127];
    }
}

static int process_frame(phys_ch_t *phys_ch, frame_t *f)
{
    int scr, scr2;
    uint8_t asbx, asby, fn0, fn1;
    data_frame_t data_frame;

    //	printf("Attempting descramble\n");
    int scr_ok = 0;
    for(scr = 0; scr <= 127; scr++) {
        //		printf("trying scrambling %i\n", scr);

        frame_t f_;
        memcpy(&f_, f, sizeof(f_));

        frame_descramble(&f_, scr);
        frame_diff_dec(&f_);
        frame_deinterleave(&f_);

        data_frame_t df;
        if (frame_decode_data(&f_, &df)) {
            continue;
        }

        if(df.data[0] != FRAME_TYPE_DATA) {
            //			printf("not data frame!\n");
            continue;
        }

        if(!frame_data_check_crc(&df)) {
            //			printf("crc mismatch!\n");
            continue;
        }

        scr2 = scr;
        memcpy(&data_frame, &df, sizeof(df));
        scr_ok = 1;
    }
    if(scr_ok == 1) {
        asbx = data_frame.data[67];
        asby = data_frame.data[68];
        fn0 = data_frame.data[1];
        fn1 = data_frame.data[2];
        printf("OK frame_no=%03i fn=%i%i asb=%i%i scr=%03i ", data_frame.frame_no, fn1, fn0, asbx, asby, scr2);
        print_buf(data_frame.data + 3, 64);
        multiblock_process(&data_frame, 2*fn1 + fn0);
        f->frame_no = data_frame.frame_no;
    } else {
        printf("ERR2 frame_no=%03i\n", f->frame_no);
        multiblock_reset();
        segmentation_reset();
    }

    return 0;
}
