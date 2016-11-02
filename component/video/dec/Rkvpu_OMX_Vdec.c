/*
 *
 * Copyright 2013 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * @file        Rkon2_OMX_Vdec.c
 * @brief
 * @author      csy (csy@rock-chips.com)
 * @version     2.0.0
 * @history
 *   2013.11.28 : Create
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Rockchip_OMX_Macros.h"
#include "Rockchip_OSAL_Event.h"
#include "Rkvpu_OMX_Vdec.h"
#include "Rkvpu_OMX_VdecControl.h"
#include "Rockchip_OMX_Basecomponent.h"
#include "Rockchip_OSAL_Thread.h"
#include "Rockchip_OSAL_Semaphore.h"
#include "Rockchip_OSAL_Mutex.h"
#include "Rockchip_OSAL_ETC.h"
#include "Rockchip_OSAL_Android.h"
#include "Rockchip_OSAL_RGA_Process.h"
#include <fcntl.h>
#include "vpu_mem.h"
#include <dlfcn.h>
#include <unistd.h>


#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "ROCKCHIP_VIDEO_DEC"
#define ROCKCHIP_LOG_OFF
//#define ROCKCHIP_TRACE_ON
#include "Rockchip_OSAL_Log.h"

#ifndef VPU_API_SET_IMMEDIATE_OUT
#define VPU_API_SET_IMMEDIATE_OUT 0x1000
#endif
typedef struct {
    OMX_RK_VIDEO_CODINGTYPE codec_id;
    OMX_VIDEO_CODINGTYPE     omx_id;
} CodeMap;

static const CodeMap kCodeMap[] = {
    { OMX_RK_VIDEO_CodingMPEG2, OMX_VIDEO_CodingMPEG2},
    { OMX_RK_VIDEO_CodingH263,  OMX_VIDEO_CodingH263},
    { OMX_RK_VIDEO_CodingMPEG4, OMX_VIDEO_CodingMPEG4},
    { OMX_RK_VIDEO_CodingVC1,   (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVC1},
    // { OMX_RK_VIDEO_CodingRV,    OMX_VIDEO_CodingRV},
    { OMX_RK_VIDEO_CodingAVC,   OMX_VIDEO_CodingAVC},
    { OMX_RK_VIDEO_CodingMJPEG, OMX_VIDEO_CodingMJPEG},
    { OMX_RK_VIDEO_CodingFLV1,  (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingFLV1},
    { OMX_RK_VIDEO_CodingVP8,   OMX_VIDEO_CodingVP8},
//   { OMX_RK_VIDEO_CodingVP6,   OMX_VIDEO_CodingVP6},
    { OMX_RK_VIDEO_CodingWMV,   OMX_VIDEO_CodingWMV},
//  { OMX_RK_VIDEO_CodingDIVX3, OMX_VIDEO_CodingDIVX3 },
    { OMX_RK_VIDEO_CodingHEVC,   OMX_VIDEO_CodingHEVC},
    { OMX_RK_VIDEO_CodingVP9,   OMX_VIDEO_CodingVP9},
};

int calc_plane(int width, int height)
{
    int mbX, mbY;

    mbX = (width + 15) / 16;
    mbY = (height + 15) / 16;

    /* Alignment for interlaced processing */
    mbY = (mbY + 1) / 2 * 2;

    return (mbX * 16) * (mbY * 16);
}

void UpdateFrameSize(OMX_COMPONENTTYPE *pOMXComponent)
{
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    ROCKCHIP_OMX_BASEPORT      *rockchipInputPort = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT      *rockchipOutputPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];

    if ((rockchipOutputPort->portDefinition.format.video.nFrameWidth !=
         rockchipInputPort->portDefinition.format.video.nFrameWidth) ||
        (rockchipOutputPort->portDefinition.format.video.nFrameHeight !=
         rockchipInputPort->portDefinition.format.video.nFrameHeight) ||
        (rockchipOutputPort->portDefinition.format.video.nStride !=
         rockchipInputPort->portDefinition.format.video.nStride) ||
        (rockchipOutputPort->portDefinition.format.video.nSliceHeight !=
         rockchipInputPort->portDefinition.format.video.nSliceHeight)) {
        OMX_U32 width = 0, height = 0;

        rockchipOutputPort->portDefinition.format.video.nFrameWidth =
            rockchipInputPort->portDefinition.format.video.nFrameWidth;
        rockchipOutputPort->portDefinition.format.video.nFrameHeight =
            rockchipInputPort->portDefinition.format.video.nFrameHeight;
        width = rockchipOutputPort->portDefinition.format.video.nStride =
                    rockchipInputPort->portDefinition.format.video.nStride;
        height = rockchipOutputPort->portDefinition.format.video.nSliceHeight =
                     rockchipInputPort->portDefinition.format.video.nSliceHeight;

        switch (rockchipOutputPort->portDefinition.format.video.eColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420SemiPlanar:
            if (width && height)
                rockchipOutputPort->portDefinition.nBufferSize = (width * height * 3) / 2;
            break;
        default:
            if (width && height)
                rockchipOutputPort->portDefinition.nBufferSize = width * height * 2;
            break;
        }
    }

    return;
}

OMX_BOOL Rkvpu_Check_BufferProcess_State(ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent, OMX_U32 nPortIndex)
{
    OMX_BOOL ret = OMX_FALSE;

    if ((pRockchipComponent->currentState == OMX_StateExecuting) &&
        (pRockchipComponent->pRockchipPort[nPortIndex].portState == OMX_StateIdle) &&
        (pRockchipComponent->transientState != ROCKCHIP_OMX_TransStateExecutingToIdle) &&
        (pRockchipComponent->transientState != ROCKCHIP_OMX_TransStateIdleToExecuting)) {
        ret = OMX_TRUE;
    } else {
        ret = OMX_FALSE;
    }

    return ret;
}
OMX_ERRORTYPE Rkvpu_ResetAllPortConfig(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_ERRORTYPE                  ret               = OMX_ErrorNone;
    ROCKCHIP_OMX_BASECOMPONENT      *pRockchipComponent  = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    ROCKCHIP_OMX_BASEPORT           *pRockchipInputPort  = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT           *pRockchipOutputPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];

    /* Input port */
    pRockchipInputPort->portDefinition.format.video.nFrameWidth = DEFAULT_FRAME_WIDTH;
    pRockchipInputPort->portDefinition.format.video.nFrameHeight = DEFAULT_FRAME_HEIGHT;
    pRockchipInputPort->portDefinition.format.video.nStride = 0; /*DEFAULT_FRAME_WIDTH;*/
    pRockchipInputPort->portDefinition.format.video.nSliceHeight = 0;
    pRockchipInputPort->portDefinition.nBufferSize = DEFAULT_VIDEO_INPUT_BUFFER_SIZE;
    pRockchipInputPort->portDefinition.format.video.pNativeRender = 0;
    pRockchipInputPort->portDefinition.format.video.bFlagErrorConcealment = OMX_FALSE;
    pRockchipInputPort->portDefinition.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    pRockchipInputPort->portDefinition.bEnabled = OMX_TRUE;
    pRockchipInputPort->bufferProcessType = BUFFER_COPY;
    pRockchipInputPort->portWayType = WAY2_PORT;

    /* Output port */
    pRockchipOutputPort->portDefinition.format.video.nFrameWidth = DEFAULT_FRAME_WIDTH;
    pRockchipOutputPort->portDefinition.format.video.nFrameHeight = DEFAULT_FRAME_HEIGHT;
    pRockchipOutputPort->portDefinition.format.video.nStride = 0; /*DEFAULT_FRAME_WIDTH;*/
    pRockchipOutputPort->portDefinition.format.video.nSliceHeight = 0;
    pRockchipOutputPort->portDefinition.nBufferSize = DEFAULT_VIDEO_OUTPUT_BUFFER_SIZE;
    pRockchipOutputPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    Rockchip_OSAL_Memset(pRockchipOutputPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
    Rockchip_OSAL_Strcpy(pRockchipOutputPort->portDefinition.format.video.cMIMEType, "raw/video");
    pRockchipOutputPort->portDefinition.format.video.pNativeRender = 0;
    pRockchipOutputPort->portDefinition.format.video.bFlagErrorConcealment = OMX_FALSE;
    pRockchipOutputPort->portDefinition.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    pRockchipOutputPort->portDefinition.nBufferCountActual = MAX_VIDEO_OUTPUTBUFFER_NUM;
    pRockchipOutputPort->portDefinition.nBufferCountMin = MAX_VIDEO_OUTPUTBUFFER_NUM;
    pRockchipOutputPort->portDefinition.nBufferSize = DEFAULT_VIDEO_OUTPUT_BUFFER_SIZE;
    pRockchipOutputPort->portDefinition.bEnabled = OMX_TRUE;
    pRockchipOutputPort->bufferProcessType = BUFFER_COPY | BUFFER_ANBSHARE;
    pRockchipOutputPort->portWayType = WAY2_PORT;

    return ret;
}

void Rkvpu_Wait_ProcessPause(ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent, OMX_U32 nPortIndex)
{
    ROCKCHIP_OMX_BASEPORT *rockchipOMXInputPort  = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT *rockchipOMXOutputPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT *rockchipOMXPort = NULL;

    FunctionIn();

    rockchipOMXPort = &pRockchipComponent->pRockchipPort[nPortIndex];

    if (((pRockchipComponent->currentState == OMX_StatePause) ||
         (pRockchipComponent->currentState == OMX_StateIdle) ||
         (pRockchipComponent->transientState == ROCKCHIP_OMX_TransStateLoadedToIdle) ||
         (pRockchipComponent->transientState == ROCKCHIP_OMX_TransStateExecutingToIdle)) &&
        (pRockchipComponent->transientState != ROCKCHIP_OMX_TransStateIdleToLoaded) &&
        (!CHECK_PORT_BEING_FLUSHED(rockchipOMXPort))) {

        Rockchip_OSAL_SignalWait(pRockchipComponent->pRockchipPort[nPortIndex].pauseEvent, DEF_MAX_WAIT_TIME);
        Rockchip_OSAL_SignalReset(pRockchipComponent->pRockchipPort[nPortIndex].pauseEvent);
    }

    FunctionOut();

    return;
}

OMX_BOOL Rkvpu_SendInputData(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_BOOL               ret = OMX_FALSE;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    ROCKCHIP_OMX_BASEPORT      *rockchipInputPort = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_DATABUFFER    *inputUseBuffer = &rockchipInputPort->way.port2WayDataBuffer.inputDataBuffer;
    VpuCodecContext_t *p_vpu_ctx = pVideoDec->vpu_ctx;
    FunctionIn();

    if (inputUseBuffer->dataValid == OMX_TRUE) {
        VideoPacket_t pkt;
        if (pVideoDec->bFirstFrame == OMX_TRUE) {
            OMX_U8 *extraData = NULL;
            OMX_U32 extraSize = 0;
            OMX_U32 extraFlag = 0;
            OMX_U32 enableDinterlace = 1;
            if (((inputUseBuffer->nFlags & OMX_BUFFERFLAG_EXTRADATA) == OMX_BUFFERFLAG_EXTRADATA)
                || ((inputUseBuffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) == OMX_BUFFERFLAG_CODECCONFIG)) {

                extraData = (OMX_U8 *)Rockchip_OSAL_Malloc(inputUseBuffer->dataLen);
                if (extraData == NULL) {
                    Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "malloc Extra Data fail");
                    ret = OMX_FALSE;
                    goto EXIT;
                }
                Rockchip_OSAL_Memcpy(extraData, inputUseBuffer->bufferHeader->pBuffer + inputUseBuffer->usedDataLen,
                                     inputUseBuffer->dataLen);
                extraSize = inputUseBuffer->dataLen;
                extraFlag = 1;
            }

            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "decode init");

            p_vpu_ctx->init(p_vpu_ctx, extraData, extraSize);
            // not use iep when thumbNail decode
            if (!(pVideoDec->flags & RKVPU_OMX_VDEC_THUMBNAIL)) {
                p_vpu_ctx->control(p_vpu_ctx, VPU_API_ENABLE_DEINTERLACE, &enableDinterlace);
            }
            if (pVideoDec->vpumem_handle != NULL) {
                p_vpu_ctx->control(p_vpu_ctx, VPU_API_SET_VPUMEM_CONTEXT, pVideoDec->vpumem_handle);
            }

            if (rockchipInputPort->portDefinition.format.video.bFlagErrorConcealment) {
                Rockchip_OSAL_Log(ROCKCHIP_LOG_DEBUG, "use directly output mode for media");
                RK_U32 flag = 1;
                p_vpu_ctx->control(p_vpu_ctx, VPU_API_SET_IMMEDIATE_OUT, (void*)&flag);
            }

            pVideoDec->bFirstFrame = OMX_FALSE;
            if (extraFlag) {
                ret = OMX_TRUE;
                if (extraData) {
                    Rockchip_OSAL_Free(extraData);
                    extraData = NULL;
                }
                Rkvpu_InputBufferReturn(pOMXComponent, inputUseBuffer);
                goto EXIT;
            }
        }

        if ((inputUseBuffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) {
            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "bSaveFlagEOS : OMX_TRUE");
            pRockchipComponent->bSaveFlagEOS = OMX_TRUE;
            //  if (inputUseBuffer->dataLen != 0)
        }
        Rockchip_OSAL_Memset(&pkt, 0, sizeof(VideoPacket_t));
        pkt.data =  inputUseBuffer->bufferHeader->pBuffer + inputUseBuffer->usedDataLen;
        pkt.size = inputUseBuffer->dataLen;
        if (pVideoDec->flags & RKVPU_OMX_VDEC_USE_DTS) {
            pkt.pts = VPU_API_NOPTS_VALUE;
            pkt.dts = inputUseBuffer->timeStamp;
        } else {
            pkt.pts = pkt.dts = inputUseBuffer->timeStamp;
        }
        if ((inputUseBuffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) {
            Rockchip_OSAL_Log(ROCKCHIP_LOG_INFO, "send eos");
            pkt.nFlags |= OMX_BUFFERFLAG_EOS;
        }

        Rockchip_OSAL_Log(ROCKCHIP_LOG_INFO, "decode_sendstream");
        p_vpu_ctx->decode_sendstream(p_vpu_ctx, &pkt);
        if (pkt.size != 0) {
            // Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR,"stream list full wait");
            goto EXIT;
        }
        Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, ",pkt.size:%d, pkt.dts:%lld,pkt.pts:%lld,pkt.nFlags:%d",
                          pkt.size, pkt.dts, pkt.pts, pkt.nFlags);
        Rkvpu_InputBufferReturn(pOMXComponent, inputUseBuffer);
        if (pRockchipComponent->checkTimeStamp.needSetStartTimeStamp == OMX_TRUE) {
            pRockchipComponent->checkTimeStamp.needCheckStartTimeStamp = OMX_TRUE;
            pRockchipComponent->checkTimeStamp.startTimeStamp = inputUseBuffer->timeStamp;
            pRockchipComponent->checkTimeStamp.nStartFlags = inputUseBuffer->nFlags;
            pRockchipComponent->checkTimeStamp.needSetStartTimeStamp = OMX_FALSE;
            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "first frame timestamp after seeking %lld us (%.2f secs)",
                              inputUseBuffer->timeStamp, inputUseBuffer->timeStamp / 1E6);
        }
        ret = OMX_TRUE;
    }

EXIT:
    FunctionOut();
    return ret;
}

OMX_BOOL Rkvpu_Post_OutputFrame(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_BOOL                   ret = OMX_FALSE;
    ROCKCHIP_OMX_BASECOMPONENT  *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    ROCKCHIP_OMX_BASEPORT           *pInputPort         = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT           *pOutputPort        = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];
    ROCKCHIP_OMX_DATABUFFER     *outputUseBuffer = &pOutputPort->way.port2WayDataBuffer.outputDataBuffer;
    VpuCodecContext_t           *p_vpu_ctx = pVideoDec->vpu_ctx;
    OMX_U32         pOWnBycomponetNum = Rockchip_OSAL_GetElemNum(&pOutputPort->bufferQ);
    OMX_S32 maxBufferNum = 0;
    OMX_S32 i = 0, numInOmxAl = 0, limitNum = 8;
    FunctionIn();
    if (p_vpu_ctx == NULL ||
        (pVideoDec->bFirstFrame == OMX_TRUE) ||
        (pVideoDec->bDecSendEOS == OMX_TRUE)) {
        goto EXIT;
    }
    maxBufferNum = pOutputPort->portDefinition.nBufferCountActual;
    for (i = 0; i < maxBufferNum; i++) {
        if (pOutputPort->extendBufferHeader[i].bBufferInOMX == OMX_FALSE) {
            numInOmxAl++;
        }
    }
    if (pOutputPort->bufferProcessType == BUFFER_SHARE) {
        OMX_U32 width = 0, height = 0;
        int imageSize = 0;
        OMX_S32 dec_ret = 0;
        DecoderOut_t pOutput;
        VPU_FRAME *pframe = (VPU_FRAME*)Rockchip_OSAL_Malloc(sizeof(VPU_FRAME));;
        OMX_BUFFERHEADERTYPE     *bufferHeader = NULL;
        Rockchip_OSAL_Memset(&pOutput, 0, sizeof(DecoderOut_t));
        Rockchip_OSAL_Memset(pframe, 0, sizeof(VPU_FRAME));
        pOutput.data = (unsigned char *)pframe;
        if ((numInOmxAl < limitNum) ||
            (pVideoDec->maxCount > 20)) {
            dec_ret =  p_vpu_ctx->decode_getframe(p_vpu_ctx, &pOutput);
            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "pOutput.size %d", pOutput.size);
            pVideoDec->maxCount = 0;
        } else {
            pVideoDec->maxCount++;
            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "pVideoDec 0x%x numInOmxAl %d", pVideoDec, numInOmxAl);
        }
        if (dec_ret < 0) {
            if (dec_ret == VPU_API_EOS_STREAM_REACHED) {
                outputUseBuffer->dataLen = 0;
                outputUseBuffer->remainDataLen = 0;
                outputUseBuffer->nFlags |= OMX_BUFFERFLAG_EOS;
                outputUseBuffer->timeStamp = 0;
                outputUseBuffer->dataValid = OMX_FALSE;
                ret = OMX_TRUE;
                pVideoDec->bDecSendEOS = OMX_TRUE;
                Rockchip_OSAL_Log(ROCKCHIP_LOG_INFO, "OMX_BUFFERFLAG_EOS");
            } else {
                Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_DECODER ERROR");
                pRockchipComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                             pRockchipComponent->callbackData, OMX_EventError,
                                                             OUTPUT_PORT_INDEX,
                                                             OMX_IndexParamPortDefinition, NULL);
            }
            Rkvpu_OutputBufferReturn(pOMXComponent, outputUseBuffer);
        }
        if (outputUseBuffer->dataValid == OMX_TRUE && (pOWnBycomponetNum > 1)) {
            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "commit fd to vpu 0x%x\n", outputUseBuffer->bufferHeader);
            Rockchip_OSAL_Fd2VpumemPool(pRockchipComponent, outputUseBuffer->bufferHeader);
            Rockchip_ResetDataBuffer(outputUseBuffer);
        }

        if ((pOutput.size > 0) && (!CHECK_PORT_BEING_FLUSHED(pOutputPort))) {
            OMX_COLOR_FORMATTYPE eColorFormat = Rockchip_OSAL_CheckFormat(pRockchipComponent, pframe);
            if ((pInputPort->portDefinition.format.video.nFrameWidth != pframe->DisplayWidth) ||
                (pInputPort->portDefinition.format.video.nFrameHeight != pframe->DisplayHeight)
                || (pInputPort->portDefinition.format.video.nSliceHeight != pframe->FrameHeight)
                || (pInputPort->portDefinition.format.video.nStride != (OMX_S32)pframe->FrameWidth)
                || pOutputPort->portDefinition.format.video.eColorFormat != eColorFormat) {
                Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "video.nFrameWidth %d video.nFrameHeight %d nSliceHeight %d",
                                  pInputPort->portDefinition.format.video.nFrameWidth,
                                  pInputPort->portDefinition.format.video.nFrameHeight,
                                  pInputPort->portDefinition.format.video.nSliceHeight);

                Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "video.nFrameWidth %d video.nFrameHeight %d pframe->FrameHeight %d",
                                  pframe->DisplayWidth,
                                  pframe->DisplayHeight, pframe->FrameHeight);

                pOutputPort->newCropRectangle.nWidth = pframe->DisplayWidth;
                pOutputPort->newCropRectangle.nHeight = pframe->DisplayHeight;
                pOutputPort->newPortDefinition.format.video.eColorFormat = eColorFormat;
                pOutputPort->newPortDefinition.nBufferCountActual = pOutputPort->portDefinition.nBufferCountActual;
                pOutputPort->newPortDefinition.nBufferCountMin = pOutputPort->portDefinition.nBufferCountMin;
                pInputPort->newPortDefinition.format.video.nFrameWidth = pframe->DisplayWidth;
                pInputPort->newPortDefinition.format.video.nFrameHeight = pframe->DisplayHeight;

                pInputPort->newPortDefinition.format.video.nStride         = Get_Video_HorAlign(pVideoDec->codecId, pframe->FrameWidth, pframe->FrameHeight);
                pInputPort->newPortDefinition.format.video.nSliceHeight    = Get_Video_VerAlign(pVideoDec->codecId, pframe->FrameHeight);

                Rkvpu_ResolutionUpdate(pOMXComponent);
                pRockchipComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                             pRockchipComponent->callbackData, OMX_EventPortSettingsChanged,
                                                             OUTPUT_PORT_INDEX,
                                                             OMX_IndexParamPortDefinition, NULL);
                if (pframe->vpumem.phy_addr > 0) {
                    VPUMemLink(&pframe->vpumem);
                    VPUFreeLinear(&pframe->vpumem);
                }
                Rockchip_OSAL_Free(pframe);
                Rockchip_OSAL_resetVpumemPool(pRockchipComponent);
                p_vpu_ctx->control(p_vpu_ctx, VPU_API_SET_INFO_CHANGE, NULL);
                goto EXIT;
            }

            if (pframe->ErrorInfo) {   //drop frame when this frame mark error from dec
                if (pframe->vpumem.phy_addr > 0) {
                    VPUMemLink(&pframe->vpumem);
                    VPUFreeLinear(&pframe->vpumem);
                }
                goto EXIT;
            }

            bufferHeader = Rockchip_OSAL_Fd2OmxBufferHeader(pOutputPort, VPUMemGetFD(&pframe->vpumem), pframe);
            if (bufferHeader != NULL) {
                if (pVideoDec->bStoreMetaData == OMX_TRUE) {
                    bufferHeader->nFilledLen = bufferHeader->nAllocLen;
                    Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "nfill len %d", bufferHeader->nFilledLen);
                } else {
                    bufferHeader->nFilledLen = pframe->DisplayHeight * pframe->DisplayWidth * 3 / 2;
                }
                bufferHeader->nOffset    = 0;
                if ((VPU_API_ERR)pOutput.nFlags == VPU_API_EOS_STREAM_REACHED) {
                    bufferHeader->nFlags |= OMX_BUFFERFLAG_EOS;
                    pVideoDec->bDecSendEOS = OMX_TRUE;
                } else {
                    bufferHeader->nFlags     = 0;
                }
                bufferHeader->nTimeStamp = pOutput.timeUs;
                Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "Rkvpu_OutputBufferReturn %lld", pOutput.timeUs);
            } else {
                VPUMemLink(&pframe->vpumem);
                VPUFreeLinear(&pframe->vpumem);
                Rockchip_OSAL_Free(pframe);
                goto EXIT;
            }

            if ((bufferHeader->nFilledLen > 0) ||
                ((bufferHeader->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) ||
                (CHECK_PORT_BEING_FLUSHED(pOutputPort))) {
                Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "Rkvpu_OutputBufferReturn");
                Rockchip_OMX_OutputBufferReturn(pOMXComponent, bufferHeader);
            }

            ret = OMX_TRUE;
        } else if (CHECK_PORT_BEING_FLUSHED(pOutputPort)) {
            if (pOutput.size && (pframe->vpumem.phy_addr > 0)) {
                VPUMemLink(&pframe->vpumem);
                VPUFreeLinear(&pframe->vpumem);
                Rockchip_OSAL_Free(pframe);
            }
            outputUseBuffer->dataLen = 0;
            outputUseBuffer->remainDataLen = 0;
            outputUseBuffer->nFlags = 0;
            outputUseBuffer->timeStamp = 0;
            ret = OMX_TRUE;
            Rkvpu_OutputBufferReturn(pOMXComponent, outputUseBuffer);
        } else {
            //Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "output buffer is smaller than decoded data size Out Length");
            //pRockchipComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
            //                                        pRockchipComponent->callbackData,
            //                                        OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            if (pframe != NULL) {
                Rockchip_OSAL_Free(pframe);
                pframe = NULL;
            }
            ret = OMX_FALSE;
        }
    } else {
        if (outputUseBuffer->dataValid == OMX_TRUE) {
            OMX_U32 width = 0, height = 0;
            int imageSize = 0;
            int ret = 0;
            DecoderOut_t pOutput;
            VPU_FRAME pframe;
            Rockchip_OSAL_Memset(&pOutput, 0, sizeof(DecoderOut_t));
            Rockchip_OSAL_Memset(&pframe, 0, sizeof(VPU_FRAME));
            pOutput.data = (unsigned char *)&pframe;
            ret =  p_vpu_ctx->decode_getframe(p_vpu_ctx, &pOutput);
            if (ret < 0) {
                if (ret == VPU_API_EOS_STREAM_REACHED) {
                    outputUseBuffer->dataLen = 0;
                    outputUseBuffer->remainDataLen = 0;
                    outputUseBuffer->nFlags |= OMX_BUFFERFLAG_EOS;
                    outputUseBuffer->timeStamp = 0;
                    outputUseBuffer->dataValid = OMX_FALSE;
                    ret = OMX_TRUE;
                    pVideoDec->bDecSendEOS = OMX_TRUE;
                    Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_BUFFERFLAG_EOS");
                } else {
                    Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_DECODER ERROR");
                    pRockchipComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                                 pRockchipComponent->callbackData, OMX_EventError,
                                                                 OUTPUT_PORT_INDEX,
                                                                 OMX_IndexParamPortDefinition, NULL);
                }
                Rkvpu_OutputBufferReturn(pOMXComponent, outputUseBuffer);
            }

            if ((pOutput.size > 0) && (!CHECK_PORT_BEING_FLUSHED(pOutputPort))) {
                if (pInputPort->portDefinition.format.video.nFrameWidth != pframe.DisplayWidth ||
                    pInputPort->portDefinition.format.video.nFrameHeight != pframe.DisplayHeight) {

                    pOutputPort->newCropRectangle.nWidth = pframe.DisplayWidth;
                    pOutputPort->newCropRectangle.nHeight = pframe.DisplayHeight;
                    pOutputPort->newPortDefinition.nBufferCountActual = pOutputPort->portDefinition.nBufferCountActual;
                    pOutputPort->newPortDefinition.nBufferCountMin = pOutputPort->portDefinition.nBufferCountMin;
                    pInputPort->newPortDefinition.format.video.nFrameWidth = pframe.DisplayWidth;
                    pInputPort->newPortDefinition.format.video.nFrameHeight = pframe.DisplayHeight;
                    pInputPort->newPortDefinition.format.video.nStride         = pframe.DisplayWidth;
                    pInputPort->newPortDefinition.format.video.nSliceHeight    = pframe.DisplayHeight;

                    Rkvpu_ResolutionUpdate(pOMXComponent);
                    pRockchipComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                                                                 pRockchipComponent->callbackData, OMX_EventPortSettingsChanged,
                                                                 OUTPUT_PORT_INDEX,
                                                                 OMX_IndexParamPortDefinition, NULL);
                    if (pframe.vpumem.phy_addr > 0) {
                        VPUMemLink(&pframe.vpumem);
                        VPUFreeLinear(&pframe.vpumem);
                    }
                    p_vpu_ctx->control(p_vpu_ctx, VPU_API_SET_INFO_CHANGE, NULL);
                    goto EXIT;

                }

                if (!pframe.vpumem.phy_addr) { /*in mpp process may be notify a null frame for info change*/
                    p_vpu_ctx->control(p_vpu_ctx, VPU_API_SET_INFO_CHANGE, NULL);
                    goto EXIT;
                }

                Rkvpu_Frame2Outbuf(pOMXComponent, outputUseBuffer->bufferHeader, &pframe);
                outputUseBuffer->remainDataLen = pframe.DisplayHeight * pframe.DisplayWidth * 3 / 2;
                outputUseBuffer->timeStamp = pOutput.timeUs;
                if (VPU_API_EOS_STREAM_REACHED == (VPU_API_ERR)pOutput.nFlags) {
                    outputUseBuffer->nFlags |= OMX_BUFFERFLAG_EOS;
                    pVideoDec->bDecSendEOS = OMX_TRUE;
                    Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_BUFFERFLAG_EOS");
                }
                if ((outputUseBuffer->remainDataLen > 0) ||
                    ((outputUseBuffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS) ||
                    (CHECK_PORT_BEING_FLUSHED(pOutputPort))) {
                    Rockchip_OSAL_Log(ROCKCHIP_LOG_INFO, "Rkvpu_OutputBufferReturn");
                    Rkvpu_OutputBufferReturn(pOMXComponent, outputUseBuffer);
                }
                ret = OMX_TRUE;
            } else if (CHECK_PORT_BEING_FLUSHED(pOutputPort)) {
                if (pOutput.size) {
                    VPUMemLink(&pframe.vpumem);
                    VPUFreeLinear(&pframe.vpumem);
                }
                outputUseBuffer->dataLen = 0;
                outputUseBuffer->remainDataLen = 0;
                outputUseBuffer->nFlags = 0;
                outputUseBuffer->timeStamp = 0;
                ret = OMX_TRUE;
                Rkvpu_OutputBufferReturn(pOMXComponent, outputUseBuffer);
            } else {
                //Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "output buffer is smaller than decoded data size Out Length");
                //pRockchipComponent->pCallbacks->EventHandler((OMX_HANDLETYPE)pOMXComponent,
                //                                        pRockchipComponent->callbackData,
                //                                        OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                ret = OMX_FALSE;
            }
        } else {
            ret = OMX_FALSE;
        }
    }
EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Rkvpu_OMX_InputBufferProcess(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    ROCKCHIP_OMX_BASEPORT      *rockchipInputPort = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT      *rockchipOutputPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];
    ROCKCHIP_OMX_DATABUFFER    *srcInputUseBuffer = &rockchipInputPort->way.port2WayDataBuffer.inputDataBuffer;
    OMX_BOOL               bCheckInputData = OMX_FALSE;
    OMX_BOOL               bValidCodecData = OMX_FALSE;

    FunctionIn();

    while (!pVideoDec->bExitBufferProcessThread) {
        Rockchip_OSAL_SleepMillisec(0);
        Rkvpu_Wait_ProcessPause(pRockchipComponent, INPUT_PORT_INDEX);
        Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "Rkvpu_Check_BufferProcess_State in");
        while ((Rkvpu_Check_BufferProcess_State(pRockchipComponent, INPUT_PORT_INDEX)) &&
               (!pVideoDec->bExitBufferProcessThread)) {

            Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "Rkvpu_OMX_InputBufferProcess in");

            if ((CHECK_PORT_BEING_FLUSHED(rockchipInputPort)) ||
                (((ROCKCHIP_OMX_EXCEPTION_STATE)rockchipOutputPort->exceptionFlag != GENERAL_STATE) && ((ROCKCHIP_OMX_ERRORTYPE)ret == OMX_ErrorInputDataDecodeYet)))
                break;

            if (rockchipInputPort->portState != OMX_StateIdle)
                break;

            Rockchip_OSAL_MutexLock(srcInputUseBuffer->bufferMutex);
            if ((ROCKCHIP_OMX_ERRORTYPE)ret != OMX_ErrorInputDataDecodeYet) {
                if ((srcInputUseBuffer->dataValid != OMX_TRUE) &&
                    (!CHECK_PORT_BEING_FLUSHED(rockchipInputPort))) {

                    ret = Rkvpu_InputBufferGetQueue(pRockchipComponent);
                    if (ret != OMX_ErrorNone) {
                        Rockchip_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
                        break;
                    }
                }

                if (srcInputUseBuffer->dataValid == OMX_TRUE) {
                    if (Rkvpu_SendInputData(hComponent) != OMX_TRUE) {
                        Rockchip_OSAL_Log(ROCKCHIP_LOG_TRACE, "stream list is full");
                        Rockchip_OSAL_SleepMillisec(5);
                    }
                }
                if (CHECK_PORT_BEING_FLUSHED(rockchipInputPort)) {
                    Rockchip_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
                    break;
                }
            }
            Rockchip_OSAL_MutexUnlock(srcInputUseBuffer->bufferMutex);
            if ((ROCKCHIP_OMX_ERRORTYPE)ret == OMX_ErrorCodecInit)
                pVideoDec->bExitBufferProcessThread = OMX_TRUE;
        }
    }

EXIT:

    FunctionOut();

    return ret;
}


OMX_ERRORTYPE Rkvpu_OMX_OutputBufferProcess(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    ROCKCHIP_OMX_BASEPORT      *rockchipOutputPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];
    ROCKCHIP_OMX_DATABUFFER    *dstOutputUseBuffer = &rockchipOutputPort->way.port2WayDataBuffer.outputDataBuffer;

    FunctionIn();

    while (!pVideoDec->bExitBufferProcessThread) {
        Rockchip_OSAL_SleepMillisec(0);
        Rkvpu_Wait_ProcessPause(pRockchipComponent, OUTPUT_PORT_INDEX);

        while ((Rkvpu_Check_BufferProcess_State(pRockchipComponent, OUTPUT_PORT_INDEX)) &&
               (!pVideoDec->bExitBufferProcessThread)) {

            if (CHECK_PORT_BEING_FLUSHED(rockchipOutputPort))
                break;

            Rockchip_OSAL_MutexLock(dstOutputUseBuffer->bufferMutex);
            if ((dstOutputUseBuffer->dataValid != OMX_TRUE) &&
                (!CHECK_PORT_BEING_FLUSHED(rockchipOutputPort))) {

                Rockchip_OSAL_Log(ROCKCHIP_LOG_INFO, "Rkvpu_OutputBufferGetQueue");
                ret = Rkvpu_OutputBufferGetQueue(pRockchipComponent);
                if (ret != OMX_ErrorNone) {
                    Rockchip_OSAL_MutexUnlock(dstOutputUseBuffer->bufferMutex);
                    break;
                }
            }

            if (dstOutputUseBuffer->dataValid == OMX_TRUE) {
                if (Rkvpu_Post_OutputFrame(pOMXComponent) != OMX_TRUE) {
                    Rockchip_OSAL_SleepMillisec(8);
                }
            }

            /* reset outputData */
            Rockchip_OSAL_MutexUnlock(dstOutputUseBuffer->bufferMutex);
        }
    }

EXIT:

    FunctionOut();

    return ret;
}

static OMX_ERRORTYPE Rkvpu_OMX_InputProcessThread(OMX_PTR threadData)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = NULL;
    ROCKCHIP_OMX_MESSAGE       *message = NULL;

    FunctionIn();

    if (threadData == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)threadData;
    ret = Rockchip_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }
    pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    Rkvpu_OMX_InputBufferProcess(pOMXComponent);

    Rockchip_OSAL_ThreadExit(NULL);

EXIT:
    FunctionOut();

    return ret;
}

static OMX_ERRORTYPE Rkvpu_OMX_OutputProcessThread(OMX_PTR threadData)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = NULL;
    ROCKCHIP_OMX_MESSAGE       *message = NULL;

    FunctionIn();

    if (threadData == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)threadData;
    ret = Rockchip_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }
    pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    Rkvpu_OMX_OutputBufferProcess(pOMXComponent);

    Rockchip_OSAL_ThreadExit(NULL);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Rkvpu_OMX_BufferProcess_Create(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;

    FunctionIn();

    pVideoDec->bExitBufferProcessThread = OMX_FALSE;

    ret = Rockchip_OSAL_ThreadCreate(&pVideoDec->hOutputThread,
                                     Rkvpu_OMX_OutputProcessThread,
                                     pOMXComponent);

    if (ret == OMX_ErrorNone)
        ret = Rockchip_OSAL_ThreadCreate(&pVideoDec->hInputThread,
                                         Rkvpu_OMX_InputProcessThread,
                                         pOMXComponent);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Rkvpu_OMX_BufferProcess_Terminate(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    OMX_S32                countValue = 0;
    unsigned int           i = 0;

    FunctionIn();

    pVideoDec->bExitBufferProcessThread = OMX_TRUE;

    Rockchip_OSAL_Get_SemaphoreCount(pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX].bufferSemID, &countValue);
    if (countValue == 0)
        Rockchip_OSAL_SemaphorePost(pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX].bufferSemID);

    Rockchip_OSAL_SignalSet(pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX].pauseEvent);
    Rockchip_OSAL_ThreadTerminate(pVideoDec->hInputThread);
    pVideoDec->hInputThread = NULL;

    Rockchip_OSAL_Get_SemaphoreCount(pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX].bufferSemID, &countValue);
    if (countValue == 0)
        Rockchip_OSAL_SemaphorePost(pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX].bufferSemID);


    Rockchip_OSAL_SignalSet(pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX].pauseEvent);

    Rockchip_OSAL_SignalSet(pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX].pauseEvent);
    Rockchip_OSAL_ThreadTerminate(pVideoDec->hOutputThread);
    pVideoDec->hOutputThread = NULL;

    pRockchipComponent->checkTimeStamp.needSetStartTimeStamp = OMX_FALSE;
    pRockchipComponent->checkTimeStamp.needCheckStartTimeStamp = OMX_FALSE;

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE omx_open_vpudec_context(RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec)
{
    pVideoDec->rkapi_hdl = dlopen("/system/lib/libvpu.so", RTLD_LAZY);
    pVideoDec->bOld_api = OMX_FALSE;
    if (pVideoDec->rkapi_hdl == NULL) {
        return OMX_ErrorHardware;
    }
    pVideoDec->rkvpu_open_cxt = (OMX_S32 (*)(VpuCodecContext_t **ctx))dlsym(pVideoDec->rkapi_hdl, "vpu_open_context");
    if (pVideoDec->rkvpu_open_cxt == NULL) {
        dlclose(pVideoDec->rkapi_hdl);
        pVideoDec->rkapi_hdl = NULL;
        Rockchip_OSAL_Log(ROCKCHIP_LOG_DEBUG, "used old version lib");
        pVideoDec->rkapi_hdl = dlopen("/system/lib/librk_vpuapi.so", RTLD_LAZY);
        if (pVideoDec->rkapi_hdl == NULL) {

            Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "dll open fail system/lib/librk_vpuapi.so");
            return OMX_ErrorHardware;
        }
        pVideoDec->rkvpu_open_cxt = (OMX_S32 (*)(VpuCodecContext_t **ctx))dlsym(pVideoDec->rkapi_hdl, "vpu_open_context");

        if (pVideoDec->rkvpu_open_cxt == NULL) {
            Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "dlsym vpu_open_context fail");
            dlclose( pVideoDec->rkapi_hdl);
            return OMX_ErrorHardware;
        }
        pVideoDec->bOld_api = OMX_TRUE;
    }
    pVideoDec->rkvpu_close_cxt = (OMX_S32 (*)(VpuCodecContext_t **ctx))dlsym(pVideoDec->rkapi_hdl, "vpu_close_context");
    return OMX_ErrorNone;
}
OMX_ERRORTYPE Rkvpu_Dec_ComponentInit(OMX_COMPONENTTYPE *pOMXComponent)
{

    OMX_ERRORTYPE                  ret               = OMX_ErrorNone;
    ROCKCHIP_OMX_BASECOMPONENT      *pRockchipComponent  = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec    =  (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    OMX_RK_VIDEO_CODINGTYPE codecId = OMX_RK_VIDEO_CodingUnused;
    ROCKCHIP_OMX_BASEPORT           *pRockchipInputPort  = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    VpuCodecContext_t *p_vpu_ctx = (VpuCodecContext_t *)Rockchip_OSAL_Malloc(sizeof(VpuCodecContext_t));
    Rockchip_OSAL_Memset((void*)p_vpu_ctx, 0, sizeof(VpuCodecContext_t));
    if (omx_open_vpudec_context(pVideoDec)) {
        ret = OMX_ErrorInsufficientResources;
        goto EXIT;
    }
    {
        int32_t kNumMapEntries = sizeof(kCodeMap) / sizeof(kCodeMap[0]);
        int i = 0;
        for (i = 0; i < kNumMapEntries; i++) {
            if (kCodeMap[i].omx_id == pVideoDec->codecId) {
                codecId = kCodeMap[i].codec_id;
                break;
            }
        }
    }

    if (pVideoDec->bOld_api == OMX_FALSE) {
        p_vpu_ctx->width = pRockchipInputPort->portDefinition.format.video.nFrameWidth;
        p_vpu_ctx->height = pRockchipInputPort->portDefinition.format.video.nFrameHeight;
        p_vpu_ctx->codecType = CODEC_DECODER;

        p_vpu_ctx->videoCoding = codecId;
    } else {
        Rockchip_OSAL_Free(p_vpu_ctx);
        p_vpu_ctx = NULL;
    }

    if ( pVideoDec->rkvpu_open_cxt != NULL) {
        pVideoDec->rkvpu_open_cxt(&p_vpu_ctx);
    }

    if (p_vpu_ctx == NULL) {
        ret = OMX_ErrorInsufficientResources;
        goto EXIT;
    }

    p_vpu_ctx->enableparsing = 1;
    p_vpu_ctx->width = pRockchipInputPort->portDefinition.format.video.nFrameWidth;
    p_vpu_ctx->height = pRockchipInputPort->portDefinition.format.video.nFrameHeight;
    p_vpu_ctx->codecType = CODEC_DECODER;


    p_vpu_ctx->videoCoding = codecId;
    pVideoDec->vpu_ctx = p_vpu_ctx;

    pVideoDec->bFirstFrame = OMX_TRUE;
    pVideoDec->maxCount = 0;
    if (rga_dev_open(&pVideoDec->rga_ctx)  < 0) {
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "open rga device fail!");
    }

    /*
     ** if current stream is Div3, tell VPU_API of on2 decoder to
     ** config hardware as Div3.
    */
    /*  if (pVideoDec->flags & RKVPU_OMX_VDEC_IS_DIV3) {
          p_vpu_ctx->videoCoding = OMX_RK_VIDEO_CodingDIVX3;
      }*/

#ifdef WRITR_FILE
    pVideoDec->fp_out = fopen("data/video/dec_out.yuv", "wb");
#endif

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Rkvpu_Dec_Terminate(OMX_COMPONENTTYPE *pOMXComponent)
{
    OMX_ERRORTYPE                  ret               = OMX_ErrorNone;
    ROCKCHIP_OMX_BASECOMPONENT      *pRockchipComponent  = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    RKVPU_OMX_VIDEODEC_COMPONENT    *pVideoDec         = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;
    ROCKCHIP_OMX_BASEPORT           *pRockchipInputPort  = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    ROCKCHIP_OMX_BASEPORT           *pRockchipOutputPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];

    int i, plane;
    FunctionIn();
    if (pVideoDec && pVideoDec->vpu_ctx) {
        if (pVideoDec->rkvpu_close_cxt) {
            pVideoDec->rkvpu_close_cxt(&pVideoDec->vpu_ctx);
        }
        pVideoDec->vpu_ctx = NULL;
        if (pVideoDec->rkapi_hdl) {
            dlclose(pVideoDec->rkapi_hdl);
            pVideoDec->rkapi_hdl = NULL;
        }
    }

    if (pVideoDec->rga_ctx != NULL) {
        rga_dev_close(pVideoDec->rga_ctx);
        pVideoDec->rga_ctx = NULL;
    }
#if 1
    Rockchip_OSAL_Closevpumempool(pRockchipComponent);
#endif
    Rkvpu_ResetAllPortConfig(pOMXComponent);

EXIT:
    FunctionOut();

    return ret;
}


OMX_ERRORTYPE Rockchip_OMX_ComponentConstructor(OMX_HANDLETYPE hComponent, OMX_STRING componentName)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = NULL;
    ROCKCHIP_OMX_BASEPORT      *pRockchipPort = NULL;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = NULL;

    FunctionIn();

    if ((hComponent == NULL) || (componentName == NULL)) {
        ret = OMX_ErrorBadParameter;
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_ErrorBadParameter, Line:%d", __LINE__);
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ret = Rockchip_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_Error, Line:%d", __LINE__);
        goto EXIT;
    }

    ret = Rockchip_OMX_BaseComponent_Constructor(pOMXComponent);
    if (ret != OMX_ErrorNone) {
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_Error, Line:%d", __LINE__);
        goto EXIT;
    }

    ret = Rockchip_OMX_Port_Constructor(pOMXComponent);
    if (ret != OMX_ErrorNone) {
        Rockchip_OMX_BaseComponent_Destructor(pOMXComponent);
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_Error, Line:%d", __LINE__);
        goto EXIT;
    }

    pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;

    pVideoDec = Rockchip_OSAL_Malloc(sizeof(RKVPU_OMX_VIDEODEC_COMPONENT));
    if (pVideoDec == NULL) {
        Rockchip_OMX_BaseComponent_Destructor(pOMXComponent);
        ret = OMX_ErrorInsufficientResources;
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_ErrorInsufficientResources, Line:%d", __LINE__);
        goto EXIT;
    }

    Rockchip_OSAL_Memset(pVideoDec, 0, sizeof(RKVPU_OMX_VIDEODEC_COMPONENT));

    pRockchipComponent->componentName = (OMX_STRING)Rockchip_OSAL_Malloc(MAX_OMX_COMPONENT_NAME_SIZE);
    if (pRockchipComponent->componentName == NULL) {
        Rockchip_OMX_ComponentDeInit(hComponent);
        ret = OMX_ErrorInsufficientResources;
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "OMX_ErrorInsufficientResources, Line:%d", __LINE__);
        goto EXIT;
    }
    Rockchip_OSAL_Memset(pRockchipComponent->componentName, 0, MAX_OMX_COMPONENT_NAME_SIZE);
    pVideoDec->bReconfigDPB = OMX_FALSE;
    pRockchipComponent->hComponentHandle = (OMX_HANDLETYPE)pVideoDec;

    pRockchipComponent->bSaveFlagEOS = OMX_FALSE;
    pRockchipComponent->nRkFlags = 0;
    pRockchipComponent->bBehaviorEOS = OMX_FALSE;
    pVideoDec->bDecSendEOS = OMX_FALSE;
    pVideoDec->bPvr_Flag = OMX_FALSE;
    pRockchipComponent->bMultiThreadProcess = OMX_TRUE;
    pRockchipComponent->codecType = HW_VIDEO_DEC_CODEC;

    pVideoDec->bFirstFrame = OMX_TRUE;

    pVideoDec->vpumem_handle = NULL;

    /* Set componentVersion */
    pRockchipComponent->componentVersion.s.nVersionMajor = VERSIONMAJOR_NUMBER;
    pRockchipComponent->componentVersion.s.nVersionMinor = VERSIONMINOR_NUMBER;
    pRockchipComponent->componentVersion.s.nRevision     = REVISION_NUMBER;
    pRockchipComponent->componentVersion.s.nStep         = STEP_NUMBER;
    /* Set specVersion */
    pRockchipComponent->specVersion.s.nVersionMajor = VERSIONMAJOR_NUMBER;
    pRockchipComponent->specVersion.s.nVersionMinor = VERSIONMINOR_NUMBER;
    pRockchipComponent->specVersion.s.nRevision     = REVISION_NUMBER;
    pRockchipComponent->specVersion.s.nStep         = STEP_NUMBER;
    /* Input port */

    pRockchipPort = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    pRockchipPort->portDefinition.nBufferCountActual = MAX_VIDEO_INPUTBUFFER_NUM;
    pRockchipPort->portDefinition.nBufferCountMin = MAX_VIDEO_INPUTBUFFER_NUM;
    pRockchipPort->portDefinition.nBufferSize = 0;
    pRockchipPort->portDefinition.eDomain = OMX_PortDomainVideo;
    pRockchipPort->portDefinition.format.video.nFrameWidth = DEFAULT_FRAME_WIDTH;
    pRockchipPort->portDefinition.format.video.nFrameHeight = DEFAULT_FRAME_HEIGHT;
    pRockchipPort->portDefinition.format.video.nStride = 0; /*DEFAULT_FRAME_WIDTH;*/
    pRockchipPort->portDefinition.format.video.nSliceHeight = 0;
    pRockchipPort->portDefinition.nBufferSize = DEFAULT_VIDEO_INPUT_BUFFER_SIZE;
    pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;

    pRockchipPort->portDefinition.format.video.cMIMEType =  Rockchip_OSAL_Malloc(MAX_OMX_MIMETYPE_SIZE);
    Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
    pRockchipPort->portDefinition.format.video.pNativeRender = 0;
    pRockchipPort->portDefinition.format.video.bFlagErrorConcealment = OMX_FALSE;
    pRockchipPort->portDefinition.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    pRockchipPort->portDefinition.bEnabled = OMX_TRUE;
    pRockchipPort->portWayType = WAY2_PORT;

    /* Output port */
    pRockchipPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];
    pRockchipPort->portDefinition.nBufferCountActual = MAX_VIDEO_OUTPUTBUFFER_NUM;
    pRockchipPort->portDefinition.nBufferCountMin = MAX_VIDEO_OUTPUTBUFFER_NUM;
    pRockchipPort->portDefinition.nBufferSize = DEFAULT_VIDEO_OUTPUT_BUFFER_SIZE;
    pRockchipPort->portDefinition.eDomain = OMX_PortDomainVideo;
    pRockchipPort->portDefinition.format.video.nFrameWidth = DEFAULT_FRAME_WIDTH;
    pRockchipPort->portDefinition.format.video.nFrameHeight = DEFAULT_FRAME_HEIGHT;
    pRockchipPort->portDefinition.format.video.nStride = 0; /*DEFAULT_FRAME_WIDTH;*/
    pRockchipPort->portDefinition.format.video.nSliceHeight = 0;
    pRockchipPort->portDefinition.nBufferSize = DEFAULT_VIDEO_OUTPUT_BUFFER_SIZE;
    pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;

    pRockchipPort->portDefinition.format.video.cMIMEType = Rockchip_OSAL_Malloc(MAX_OMX_MIMETYPE_SIZE);
    Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "raw/video");
    pRockchipPort->portDefinition.format.video.pNativeRender = 0;
    pRockchipPort->portDefinition.format.video.bFlagErrorConcealment = OMX_FALSE;
    pRockchipPort->portDefinition.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    pRockchipPort->portDefinition.bEnabled = OMX_TRUE;
    pRockchipPort->portWayType = WAY2_PORT;
    pRockchipPort->portDefinition.eDomain = OMX_PortDomainVideo;
    pRockchipPort->bufferProcessType = BUFFER_COPY | BUFFER_ANBSHARE;

    pRockchipPort->processData.extInfo = (OMX_PTR)Rockchip_OSAL_Malloc(sizeof(DECODE_CODEC_EXTRA_BUFFERINFO));
    Rockchip_OSAL_Memset(((char *)pRockchipPort->processData.extInfo), 0, sizeof(DECODE_CODEC_EXTRA_BUFFERINFO));
    {
        int i = 0;
        DECODE_CODEC_EXTRA_BUFFERINFO *pBufferInfo = NULL;
        pBufferInfo = (DECODE_CODEC_EXTRA_BUFFERINFO *)(pRockchipPort->processData.extInfo);
    }
    pOMXComponent->UseBuffer              = &Rkvpu_OMX_UseBuffer;
    pOMXComponent->AllocateBuffer         = &Rkvpu_OMX_AllocateBuffer;
    pOMXComponent->FreeBuffer             = &Rkvpu_OMX_FreeBuffer;
    pOMXComponent->ComponentTunnelRequest = &Rkvpu_OMX_ComponentTunnelRequest;
    pOMXComponent->GetParameter           = &Rkvpu_OMX_GetParameter;
    pOMXComponent->SetParameter           = &Rkvpu_OMX_SetParameter;
    pOMXComponent->GetConfig              = &Rkvpu_OMX_GetConfig;
    pOMXComponent->SetConfig              = &Rkvpu_OMX_SetConfig;
    pOMXComponent->GetExtensionIndex      = &Rkvpu_OMX_GetExtensionIndex;
    pOMXComponent->ComponentRoleEnum      = &Rkvpu_OMX_ComponentRoleEnum;
    pOMXComponent->ComponentDeInit        = &Rockchip_OMX_ComponentDeInit;

    pRockchipComponent->rockchip_codec_componentInit      = &Rkvpu_Dec_ComponentInit;
    pRockchipComponent->rockchip_codec_componentTerminate = &Rkvpu_Dec_Terminate;

    pRockchipComponent->rockchip_AllocateTunnelBuffer = &Rkvpu_OMX_AllocateTunnelBuffer;
    pRockchipComponent->rockchip_FreeTunnelBuffer     = &Rkvpu_OMX_FreeTunnelBuffer;
    pRockchipComponent->rockchip_BufferProcessCreate    = &Rkvpu_OMX_BufferProcess_Create;
    pRockchipComponent->rockchip_BufferProcessTerminate = &Rkvpu_OMX_BufferProcess_Terminate;
    pRockchipComponent->rockchip_BufferFlush          = &Rkvpu_OMX_BufferFlush;

    pRockchipPort = &pRockchipComponent->pRockchipPort[INPUT_PORT_INDEX];
    if (!strcmp(componentName, RK_OMX_COMPONENT_H264_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/avc");
        pVideoDec->codecId = OMX_VIDEO_CodingAVC;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;

    } else if (!strcmp(componentName, RK_OMX_COMPONENT_H264_DRM_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/avc");
        pVideoDec->codecId = OMX_VIDEO_CodingAVC;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;

    } else if (!strcmp(componentName, RK_OMX_COMPONENT_MPEG4_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/mp4v-es");
        pVideoDec->codecId = OMX_VIDEO_CodingMPEG4;
        pRockchipPort->portDefinition.format.video.eCompressionFormat =  OMX_VIDEO_CodingMPEG4;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_H263_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/3gpp");
        pVideoDec->codecId = OMX_VIDEO_CodingH263;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingH263;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_FLV_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/flv");
        pVideoDec->codecId = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingFLV1;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingFLV1;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_MPEG2_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/mpeg2");
        pVideoDec->codecId = OMX_VIDEO_CodingMPEG2;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_RMVB_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/vnd.rn-realvideo");
        pVideoDec->codecId = OMX_VIDEO_CodingRV;
        pRockchipPort->portDefinition.format.video.eCompressionFormat =  OMX_VIDEO_CodingRV;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_VP8_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/x-vnd.on2.vp8");
        pVideoDec->codecId = OMX_VIDEO_CodingVP8;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = pVideoDec->codecId = OMX_VIDEO_CodingVP8;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_VC1_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/vc1");
        pVideoDec->codecId = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVC1;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVC1;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_WMV3_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/x-ms-wmv");
        pVideoDec->codecId = OMX_VIDEO_CodingWMV;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_VP6_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/vp6");
        pVideoDec->codecId = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVP6;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVP6;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_HEVC_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/hevc");
        pVideoDec->codecId = OMX_VIDEO_CodingHEVC;
#ifndef LOW_VRESION
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingHEVC;
#else
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_OLD_CodingHEVC;
#endif
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_MJPEG_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/mjpeg");
        pVideoDec->codecId = OMX_VIDEO_CodingMJPEG;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingMJPEG;
    } else if (!strcmp(componentName, RK_OMX_COMPONENT_VP9_DEC)) {
        Rockchip_OSAL_Memset(pRockchipPort->portDefinition.format.video.cMIMEType, 0, MAX_OMX_MIMETYPE_SIZE);
        Rockchip_OSAL_Strcpy(pRockchipPort->portDefinition.format.video.cMIMEType, "video/vp9");
        pVideoDec->codecId = OMX_VIDEO_CodingVP9;
        pRockchipPort->portDefinition.format.video.eCompressionFormat = OMX_VIDEO_CodingVP9;
    } else {
        // IL client specified an invalid component name
        Rockchip_OSAL_Log(ROCKCHIP_LOG_ERROR, "VPU Component Invalid Component Name\n");
        ret =  OMX_ErrorInvalidComponentName;
        goto EXIT;
    }
    {
        int gpu_fd = -1;
        gpu_fd = open("/dev/pvrsrvkm", O_RDWR, 0);
        if (gpu_fd > 0) {
            pVideoDec->bPvr_Flag = OMX_TRUE;
            close(gpu_fd);
        }
    }
    pRockchipComponent->currentState = OMX_StateLoaded;
EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE Rockchip_OMX_ComponentDeInit(OMX_HANDLETYPE hComponent)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    ROCKCHIP_OMX_BASECOMPONENT *pRockchipComponent = NULL;
    ROCKCHIP_OMX_BASEPORT      *pRockchipPort = NULL;
    RKVPU_OMX_VIDEODEC_COMPONENT *pVideoDec = NULL;
    int                    i = 0;

    FunctionIn();

    if (hComponent == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ret = Rockchip_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }

    if (pOMXComponent->pComponentPrivate == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    pRockchipComponent = (ROCKCHIP_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;

    pVideoDec = (RKVPU_OMX_VIDEODEC_COMPONENT *)pRockchipComponent->hComponentHandle;

//    Rockchip_OSAL_RefANB_Terminate(pVideoDec->hRefHandle);

    Rockchip_OSAL_Free(pVideoDec);
    pRockchipComponent->hComponentHandle = pVideoDec = NULL;

    pRockchipPort = &pRockchipComponent->pRockchipPort[OUTPUT_PORT_INDEX];
    if (pRockchipPort->processData.extInfo != NULL) {
        Rockchip_OSAL_Free(pRockchipPort->processData.extInfo);
        pRockchipPort->processData.extInfo = NULL;
    }

    for (i = 0; i < ALL_PORT_NUM; i++) {
        pRockchipPort = &pRockchipComponent->pRockchipPort[i];
        Rockchip_OSAL_Free(pRockchipPort->portDefinition.format.video.cMIMEType);
        pRockchipPort->portDefinition.format.video.cMIMEType = NULL;
    }

    ret = Rockchip_OMX_Port_Destructor(pOMXComponent);

    ret = Rockchip_OMX_BaseComponent_Destructor(hComponent);

EXIT:
    FunctionOut();

    return ret;
}