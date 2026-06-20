/* 桩头文件 - 模拟 video_encoder.h 用于验证编译 */
#ifndef VIDEO_ENCODER_STUB_H
#define VIDEO_ENCODER_STUB_H

#include <stdint.h>
#include <stdbool.h>
#include "screen_capture.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
    uint32_t fps;
    uint32_t gopSize;
    uint32_t profile;
    bool     lowLatency;
} VideoEncoderConfig;

typedef struct {
    uint8_t  *data;
    uint32_t  length;
    uint64_t  timestamp;
    bool      isKeyFrame;
} EncodedFrame;

typedef struct {
    uint8_t  *spsData;
    uint32_t  spsLength;
    uint8_t  *ppsData;
    uint32_t  ppsLength;
} SpsPpsData;

typedef void (*EncodedFrameCallback)(EncodedFrame *frame, void *userData);
typedef void (*SpsPpsCallback)(const SpsPpsData *spsPps, void *userData);

static inline int video_encoder_create(const VideoEncoderConfig *c) { (void)c; return 0; }
static inline void video_encoder_set_frame_callback(EncodedFrameCallback cb, void *ud) { (void)cb; (void)ud; }
static inline void video_encoder_set_sps_pps_callback(SpsPpsCallback cb, void *ud) { (void)cb; (void)ud; }
static inline int video_encoder_start(void) { return 0; }
static inline int video_encoder_encode_frame(CapturedFrame *f) { (void)f; return 0; }
static inline int video_encoder_request_key_frame(void) { return 0; }
static inline int video_encoder_set_bitrate(uint32_t b) { (void)b; return 0; }
static inline int video_encoder_stop(void) { return 0; }
static inline void video_encoder_destroy(void) {}

#endif
