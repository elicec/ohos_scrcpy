/* OpenH264 设备端探针：定位 create/GetDefaultParams/InitializeExt 崩溃点 */
#include <stdio.h>
#include <string.h>
#include "codec_api.h"

int main(void)
{
    printf("[probe] step1: WelsCreateSVCEncoder...\n"); fflush(stdout);
    ISVCEncoder enc = NULL;
    int rv = WelsCreateSVCEncoder(&enc);
    printf("[probe] step1 done: rv=%d enc=%p\n", rv, (void *)enc); fflush(stdout);
    if (rv != 0 || enc == NULL) return 1;

    printf("[probe] step2: GetDefaultParams...\n"); fflush(stdout);
    SEncParamExt param;
    memset(&param, 0, sizeof(param));
    rv = enc->GetDefaultParams(enc, &param);
    printf("[probe] step2 done: rv=%d\n", rv); fflush(stdout);

    printf("[probe] step3: InitializeExt 1080x2408@30...\n"); fflush(stdout);
    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = 1080;
    param.iPicHeight = 2408;
    param.iTargetBitrate = 4000000;
    param.iRCMode = RC_BITRATE_MODE;
    param.fMaxFrameRate = 30.0f;
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.iMultipleThreadIdc = 4;
    param.uiIntraPeriod = 60;
    param.eSpsPpsIdStrategy = CONSTANT_ID;
    param.bEnableFrameSkip = false;
    param.iEntropyCodingModeFlag = 0;
    param.iComplexityMode = LOW_COMPLEXITY;
    param.sSpatialLayers[0].iVideoWidth = 1080;
    param.sSpatialLayers[0].iVideoHeight = 2408;
    param.sSpatialLayers[0].fFrameRate = 30.0f;
    param.sSpatialLayers[0].iSpatialBitrate = 4000000;
    param.sSpatialLayers[0].iMaxSpatialBitrate = 8000000;
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    param.sSpatialLayers[0].uiLevelIdc = LEVEL_5_0;
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
    param.sSpatialLayers[0].sSliceArgument.uiSliceNum = 4;
    rv = enc->InitializeExt(enc, &param);
    printf("[probe] step3 done: rv=%d\n", rv); fflush(stdout);

    printf("[probe] step4: EncodeFrame 1 frame...\n"); fflush(stdout);
    static unsigned char nv12[1080 * 2408 * 3 / 2];
    SSourcePicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.iPicWidth = 1080;
    pic.iPicHeight = 2408;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = 1080;
    pic.iStride[1] = 540;
    pic.iStride[2] = 540;
    pic.pData[0] = nv12;
    pic.pData[1] = nv12 + 1080 * 2408;
    pic.pData[2] = nv12 + 1080 * 2408 * 5 / 4;
    pic.uiTimeStamp = 0;
    SFrameBSInfo info;
    memset(&info, 0, sizeof(info));
    rv = enc->EncodeFrame(enc, &pic, &info);
    printf("[probe] step4 done: rv=%d size=%d type=%d\n", rv,
           info.iFrameSizeInBytes, info.eFrameType); fflush(stdout);

    printf("[probe] step5: destroy\n"); fflush(stdout);
    WelsDestroySVCEncoder((ISVCEncoder *)enc);
    printf("[probe] all OK\n"); fflush(stdout);
    return 0;
}
