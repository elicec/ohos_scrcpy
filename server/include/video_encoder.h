/*
 * OHOS Scrcpy 服务端 - 视频编码模块
 * 使用 OH_VideoEncoder H.264 硬件编码
 */

#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include "screen_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编码器配置 */
typedef struct {
    uint32_t width;           /* 视频宽度 */
    uint32_t height;          /* 视频高度 */
    uint32_t bitrate;         /* 码率(bps)，默认4000000 */
    uint32_t fps;             /* 帧率，默认60 */
    uint32_t gopSize;         /* 关键帧间隔，默认60 */
    uint32_t profile;         /* H.264 profile: 1=Baseline, 2=High */
    bool     lowLatency;      /* 低延迟模式 */
} VideoEncoderConfig;

/* 编码后的帧数据 */
typedef struct {
    uint8_t  *data;           /* 编码数据 */
    uint32_t  length;         /* 数据长度 */
    uint64_t  timestamp;      /* 时间戳(微秒) */
    bool      isKeyFrame;     /* 是否关键帧 */
} EncodedFrame;

/* SPS/PPS 数据 */
typedef struct {
    uint8_t  *spsData;        /* SPS 数据 */
    uint32_t  spsLength;      /* SPS 长度 */
    uint8_t  *ppsData;        /* PPS 数据 */
    uint32_t  ppsLength;      /* PPS 长度 */
} SpsPpsData;

typedef void (*EncodedFrameCallback)(EncodedFrame *frame, void *userData);
typedef void (*SpsPpsCallback)(const SpsPpsData *spsPps, void *userData);

/* 创建编码器 */
int video_encoder_create(const VideoEncoderConfig *config);

/* 设置编码帧回调 */
void video_encoder_set_frame_callback(EncodedFrameCallback callback, void *userData);

/* 设置 SPS/PPS 回调 */
void video_encoder_set_sps_pps_callback(SpsPpsCallback callback, void *userData);

/* 启动编码器 */
int video_encoder_start(void);

/* 送入原始帧进行编码 */
int video_encoder_encode_frame(CapturedFrame *frame);

/* 请求关键帧 */
int video_encoder_request_key_frame(void);

/* 动态调整码率 */
int video_encoder_set_bitrate(uint32_t bitrate);

/* 停止编码器 */
int video_encoder_stop(void);

/* 销毁编码器 */
void video_encoder_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_ENCODER_H */
