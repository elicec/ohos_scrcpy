/*
 * OpenH264 软件编码器 C 接口封装
 *
 * OpenH264 官方仅提供 C++ 接口（ISVCEncoder 类方法调用），纯 C 无法直接
 * 使用（codec_api.h 的 C vtbl 段与实际实现不兼容，直接调用会崩溃）。
 * 本文件用 C++ 实现并导出 extern "C" 接口供 video_encoder.c 调用。
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "codec_api.h"

/* RGBA(紧凑) -> I420，BT.601 full-range */
static void rgba_to_i420(const uint8_t *rgba, uint32_t width, uint32_t height,
                         uint8_t *yPlane, uint8_t *uPlane, uint8_t *vPlane)
{
    uint32_t uvStride = width / 2;
    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *src = rgba + (size_t)row * width * 4;
        uint8_t *yRow = yPlane + (size_t)row * width;
        for (uint32_t col = 0; col < width; col++) {
            uint32_t r = src[col * 4 + 0];
            uint32_t g = src[col * 4 + 1];
            uint32_t b = src[col * 4 + 2];
            yRow[col] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
    for (uint32_t row = 0; row < height / 2; row++) {
        const uint8_t *src1 = rgba + (size_t)(row * 2) * width * 4;
        const uint8_t *src2 = src1 + (size_t)width * 4;
        uint8_t *uRow = uPlane + (size_t)row * uvStride;
        uint8_t *vRow = vPlane + (size_t)row * uvStride;
        for (uint32_t col = 0; col < uvStride; col++) {
            uint32_t c0 = col * 8;
            uint32_t c1 = c0 + 4;
            int32_t r = (src1[c0] + src1[c1] + src2[c0] + src2[c1]) / 4;
            int32_t g = (src1[c0 + 1] + src1[c1 + 1] + src2[c0 + 1] + src2[c1 + 1]) / 4;
            int32_t b = (src1[c0 + 2] + src1[c1 + 2] + src2[c0 + 2] + src2[c1 + 2]) / 4;
            uRow[col] = (uint8_t)(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
            vRow[col] = (uint8_t)(((128 * r - 107 * g - 21 * b) >> 8) + 128);
        }
    }
}

static ISVCEncoder *g_enc = NULL;
static uint8_t *g_i420Buf = NULL;
static uint32_t g_width = 0;
static uint32_t g_height = 0;

extern "C" {

void swenc_destroy(void);

int swenc_create(uint32_t width, uint32_t height,
                 uint32_t bitrate, uint32_t fps, uint32_t gop)
{
    swenc_destroy();

    if (WelsCreateSVCEncoder(&g_enc) != 0 || g_enc == NULL) {
        return -1;
    }

    SEncParamExt param;
    memset(&param, 0, sizeof(param));
    g_enc->GetDefaultParams(&param);

    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = (int)width;
    param.iPicHeight = (int)height;
    param.iTargetBitrate = (int)bitrate;
    param.iRCMode = RC_BITRATE_MODE;
    param.fMaxFrameRate = (float)fps;
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.iMultipleThreadIdc = 4;       /* 4 线程并行编码 */
    param.uiIntraPeriod = (int)gop;
    param.eSpsPpsIdStrategy = CONSTANT_ID;
    param.bPrefixNalAddingCtrl = false;
    param.bEnableDenoise = false;
    param.bEnableFrameSkip = false;
    param.bEnableBackgroundDetection = true;
    param.bEnableAdaptiveQuant = true;
    param.bEnableLongTermReference = false;
    param.iEntropyCodingModeFlag = 0;   /* CAVLC (Baseline) */
    param.iComplexityMode = LOW_COMPLEXITY;

    SSpatialLayerConfig *sl = &param.sSpatialLayers[0];
    sl->iVideoWidth = (int)width;
    sl->iVideoHeight = (int)height;
    sl->fFrameRate = (float)fps;
    sl->iSpatialBitrate = (int)bitrate;
    sl->iMaxSpatialBitrate = (int)bitrate * 2;
    sl->uiProfileIdc = PRO_BASELINE;
    sl->uiLevelIdc = LEVEL_5_0;
    sl->sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
    sl->sSliceArgument.uiSliceNum = 4;

    if (g_enc->InitializeExt(&param) != cmResultSuccess) {
        WelsDestroySVCEncoder(g_enc);
        g_enc = NULL;
        return -1;
    }

    /* I420 缓冲：Y(w*h) + U(w/2*h/2) + V(w/2*h/2) */
    g_i420Buf = (uint8_t *)malloc((size_t)width * height * 3 / 2);
    if (g_i420Buf == NULL) {
        WelsDestroySVCEncoder(g_enc);
        g_enc = NULL;
        return -1;
    }
    g_width = width;
    g_height = height;
    return 0;
}

int swenc_encode(const uint8_t *rgba, uint64_t timestampUs,
                 void (*onFrame)(const uint8_t *data, uint32_t len,
                                 int isKeyFrame, uint64_t tsUs, void *userData),
                 void *userData)
{
    if (g_enc == NULL || g_i420Buf == NULL || rgba == NULL) {
        return -1;
    }

    uint32_t w = g_width;
    uint32_t h = g_height;
    uint8_t *yPlane = g_i420Buf;
    uint8_t *uPlane = yPlane + (size_t)w * h;
    uint8_t *vPlane = uPlane + (size_t)(w / 2) * (h / 2);
    rgba_to_i420(rgba, w, h, yPlane, uPlane, vPlane);

    SSourcePicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.iPicWidth = (int)w;
    pic.iPicHeight = (int)h;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = (int)w;
    pic.iStride[1] = (int)(w / 2);
    pic.iStride[2] = (int)(w / 2);
    pic.pData[0] = yPlane;
    pic.pData[1] = uPlane;
    pic.pData[2] = vPlane;
    pic.uiTimeStamp = (long long)(timestampUs / 1000);

    SFrameBSInfo info;
    memset(&info, 0, sizeof(info));
    if (g_enc->EncodeFrame(&pic, &info) != cmResultSuccess) {
        return -1;
    }
    if (info.iFrameSizeInBytes <= 0) {
        return 0; /* 本帧无输出 */
    }

    bool isKey = (info.eFrameType == videoFrameTypeIDR ||
                  info.eFrameType == videoFrameTypeI);
    if (onFrame) {
        onFrame(info.sLayerInfo[0].pBsBuf, (uint32_t)info.iFrameSizeInBytes,
                isKey ? 1 : 0, timestampUs, userData);
    }
    return 0;
}

void swenc_destroy(void)
{
    if (g_enc != NULL) {
        WelsDestroySVCEncoder(g_enc);
        g_enc = NULL;
    }
    if (g_i420Buf != NULL) {
        free(g_i420Buf);
        g_i420Buf = NULL;
    }
    g_width = 0;
    g_height = 0;
}

} /* extern "C" */
