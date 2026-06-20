/*
 * OHOS Scrcpy 客户端 - 视频解码模块
 * 使用 FFmpeg + DXVA2 硬件加速解码 H.264
 */

#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 解码后的帧数据 */
typedef struct {
    uint8_t  *data[4];      /* YUV 平面数据 */
    int       linesize[4];  /* 每行字节数 */
    int       width;         /* 帧宽度 */
    int       height;        /* 帧高度 */
    uint64_t  timestamp;     /* 时间戳 */
} DecodedFrame;

typedef void (*DecodedFrameCallback)(DecodedFrame *frame, void *userData);

/* 创建解码器 */
int video_decoder_create(void);

/* 设置解码帧回调 */
void video_decoder_set_callback(DecodedFrameCallback callback, void *userData);

/* 配置 SPS/PPS */
int video_decoder_set_sps_pps(const uint8_t *sps, uint32_t spsLen,
                              const uint8_t *pps, uint32_t ppsLen);

/* 送入编码数据解码 */
int video_decoder_decode(const uint8_t *data, uint32_t length,
                         uint64_t timestamp, bool isKeyFrame);

/* 刷新解码器 */
int video_decoder_flush(void);

/* 获取视频尺寸 */
void video_decoder_get_size(int *width, int *height);

/* 销毁解码器 */
void video_decoder_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_DECODER_H */
