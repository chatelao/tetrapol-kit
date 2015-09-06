#pragma once

#include <stdint.h>

enum {
    FRAME_NO_UNKNOWN = -1,
};

typedef enum {
    FRAME_TYPE_AUTO = -1,
    FRAME_TYPE_VOICE = 0,
    FRAME_TYPE_DATA = 1,
    FRAME_TYPE_HR_DATA,
    FRAME_TYPE_RANDOM_ACCESS,
    FRAME_TYPE_TRAINING,
    FRAME_TYPE_DM_EMERGENCY,
    FRAME_TYPE_SCH_TI,
} frame_type_t;

/// PAS 0001-2 6.1
typedef struct {
    union {
        struct {
            uint8_t d;
            uint8_t voice1[20];
            uint8_t asb[2];
        };
        uint8_t crc_data[23];
    };
    uint8_t crc[3];
    uint8_t voice2[100];
} frame_voice_t;

/// PAS 0001-2 6.2
typedef struct {
    union {
        struct {
            uint8_t d;
            uint8_t data[66];
            uint8_t asb[2];
        };
        uint8_t crc_data[69];
    };
    uint8_t crc[5];
    uint8_t zero[2];
} frame_data_t;

/// PAS 0001-2 6.3
typedef struct {
    uint8_t fn[2];
    uint8_t data[92];
    uint8_t asb[2];
} frame_hr_data_t;

/// PAS 0001-2 6.4
typedef struct {
    // TODO
} frame_rach_t;

/// PAS 0001-2 6.5
typedef struct {
    // TODO
} frame_training_t;

/// PAS 0001-2 6.6
typedef struct {
    // TODO
} frame_direct_emergecy_t;

typedef struct {
    union {
        uint8_t d;
        uint8_t blob_[0];    ///< used to copy data into frame structure
        frame_voice_t voice;
        frame_data_t data;
        frame_hr_data_t hr_data;
        frame_rach_t rach;
        frame_training_t trainign;
        frame_direct_emergecy_t direct_emergecy;
    };
    int fr_type;
    /**
      0 when frame is ok
      -1 when CRC does not match
      number of detected errors otherwise
      */
    int errors;
} frame_t;


// == Frame decoder ==
typedef struct frame_decoder_priv_t frame_decoder_t;

frame_decoder_t *frame_decoder_create(int band, int scr, int fr_type);
void frame_decoder_destroy(frame_decoder_t *fd);
void frame_decoder_reset(frame_decoder_t *fd, int band, int scr, int fr_type);
void frame_decoder_set_scr(frame_decoder_t *fd, int scr);

/**
  Decode frame from frame data.

  @param fd
  @param fr Pointer to preallocated frame_t structure.
  @param data frame data
  */
void frame_decoder_decode(frame_decoder_t *fd, frame_t *fr, const uint8_t *fr_data);

