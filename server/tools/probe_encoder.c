/*
 * 设备端编码器探针：列举 codec 服务可见的全部组件 + 尝试常见 Unisoc 组件名
 */
#include <stdio.h>
#include <string.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avformat.h>

int main(void)
{
    printf("=== AVC encoder capability ===\n");
    OH_AVCapability *cap = OH_AVCodec_GetCapability(OH_AVCODEC_MIMETYPE_VIDEO_AVC, true);
    printf("  GetCapability(AVC, enc): %p name=%s\n", (void *)cap,
           cap ? OH_AVCapability_GetName(cap) : "-");

    printf("=== Try CreateByName ===\n");
    const char *names[] = {
        "OMX.sprd.avc.encoder",
        "OMX.sprd.h264.encoder",
        "omx.sprd.avc.encoder",
        "OMX.Unisoc.avc.encoder",
        "OMX.google.avc.encoder",
        "ohom.avc.encoder",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        OH_AVCodec *c = OH_VideoEncoder_CreateByName(names[i]);
        printf("  CreateByName(%s): %p\n", names[i], (void *)c);
        if (c != NULL) OH_VideoEncoder_Destroy(c);
    }

    printf("=== Try CreateByMime ===\n");
    OH_AVCodec *c = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    printf("  CreateByMime(AVC): %p\n", (void *)c);
    if (c != NULL) OH_VideoEncoder_Destroy(c);

    return 0;
}
