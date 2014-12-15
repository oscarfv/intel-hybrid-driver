/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *     Zhao Yakui <yakui.zhao@intel.com>
 *
 */

#include "intel_hybrid_hostvld_vp9_parser.h"
#include "intel_hybrid_hostvld_vp9_loopfilter.h"
#include "intel_hybrid_hostvld_vp9_context.h"
#include "intel_hybrid_hostvld_vp9_engine.h"


#define VP9_SafeFreeMemory(ptr)               \
    if (ptr) free(ptr);             \

#define INTEL_HOSTVLD_VP9_THREAD_NUM     1
#define INTEL_HOSTVLD_VP9_HOSTBUF_NUM    2
#define INTEL_HOSTVLD_VP9_DDIBUF_NUM     INTEL_MT_DXVA_BUF_NUM
#define INTEL_HOSTVLD_VP9_SEM_QUEUE_SIZE 128

#define VP9_REALLOCATE_ABOVE_CTX_BUFFER(pAboveCtxBuffer, size) \
do {                                                           \
    if (pAboveCtxBuffer)                                       \
    {                                                          \
        free(pAboveCtxBuffer);                       \
    }                                                          \
    pAboveCtxBuffer = (PUINT8)calloc(1, size);    \
} while(0)

#define VP9_REALLOCATE_HOSTVLD_1D_BUFFER_UINT8(pHostvldBuffer, dwBufferSize)  \
do                                                                            \
{                                                                             \
    if ((pHostvldBuffer)->pu8Buffer)                                            \
    {                                                                         \
        free((pHostvldBuffer)->pu8Buffer);                            \
    }                                                                         \
    (pHostvldBuffer)->dwSize = dwBufferSize;                                    \
    (pHostvldBuffer)->pu8Buffer = (PUINT8)calloc(1, (dwBufferSize)); \
} while (0)

VAStatus Intel_HostvldVp9_Execute_MT (
    INTEL_HOSTVLD_VP9_HANDLE         hHostVld);

static VAStatus Intel_HostvldVp9_GetPartitions(
    PINTEL_HOSTVLD_VP9_FRAME_INFO    pFrameInfo, 
    PINTEL_HOSTVLD_VP9_VIDEO_BUFFER  pVideoBuffer, 
    PINTEL_VP9_PIC_PARAMS            pPicParams)
{
    VAStatus  eStatus     = VA_STATUS_SUCCESS;


    if (pPicParams->UncompressedHeaderLengthInBytes >= 2)
    pFrameInfo->FirstPartition.pu8Buffer  = 
        pVideoBuffer->pbBitsData + pPicParams->UncompressedHeaderLengthInBytes;
    pFrameInfo->FirstPartition.dwSize     = pPicParams->FirstPartitionSize;

    pFrameInfo->SecondPartition.pu8Buffer = 
        pFrameInfo->FirstPartition.pu8Buffer + pFrameInfo->FirstPartition.dwSize;
    pFrameInfo->SecondPartition.dwSize    = 
        pVideoBuffer->dwBitsSize - pPicParams->UncompressedHeaderLengthInBytes - pFrameInfo->FirstPartition.dwSize;

    return eStatus;
}

static VAStatus Intel_HostvldVp9_FillIntraFrameRefFrame(
    PINTEL_HOSTVLD_VP9_1D_BUFFER  pBufferRefFrame)
{
    PINT8       pi8Buffer;
    UINT        i;
    VAStatus  eStatus     = VA_STATUS_SUCCESS;


    pi8Buffer = (PINT8)pBufferRefFrame->pu8Buffer;

    for(i = 0; i < pBufferRefFrame->dwSize; i++)
    {
        *(pi8Buffer++) = VP9_REF_FRAME_INTRA;
        *(pi8Buffer++) = VP9_REF_FRAME_INTRA;
    }

finish:
    return eStatus;
}

VAStatus Intel_HostvldVp9_Create (
    PINTEL_HOSTVLD_VP9_HANDLE        phHostVld,
    PINTEL_HOSTVLD_VP9_CALLBACKS     pCallbacks)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState;
    PINTEL_HOSTVLD_VP9_TILE_STATE    pTileState;
    PINTEL_HOSTVLD_VP9_VIDEO_BUFFER  pVideoBuffer, pVideoBufferData;
    PINTEL_HOSTVLD_VP9_FRAME_CONTEXT pContext;
    uint32_t                               dwThreadNumber;
    uint32_t                               dwBufferNumber;
    uint32_t                               dwDDIBufNumber;
    uint32_t                                i           = 0;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;


    pVp9HostVld = (PINTEL_HOSTVLD_VP9_STATE)calloc(1, sizeof(*pVp9HostVld));
    *phHostVld  = (INTEL_HOSTVLD_VP9_HANDLE)pVp9HostVld;
    

    dwThreadNumber = INTEL_HOSTVLD_VP9_THREAD_NUM;

    pVp9HostVld->pfnDeblockCb       = pCallbacks->pfnHostVldDeblockCb;
    pVp9HostVld->pfnRenderCb        = pCallbacks->pfnHostVldRenderCb;
    pVp9HostVld->pfnSyncCb          = pCallbacks->pfnHostVldSyncCb;
    pVp9HostVld->pvStandardState    = pCallbacks->pvStandardState;
    pVp9HostVld->dwThreadNumber     = dwThreadNumber;
    pVp9HostVld->dwBufferNumber     = INTEL_HOSTVLD_VP9_HOSTBUF_NUM;
    pVp9HostVld->dwDDIBufNumber     = dwThreadNumber;

    pthread_mutex_init(&pVp9HostVld->MutexSync, NULL);
    // Create Frame State
    pFrameState = (PINTEL_HOSTVLD_VP9_FRAME_STATE)calloc(pVp9HostVld->dwBufferNumber, sizeof(*pFrameState));
    pVp9HostVld->pFrameStateBase = pFrameState;

    for (i = 0; i < pVp9HostVld->dwBufferNumber; i++)
    {

         // Create Tile State
        pTileState = (PINTEL_HOSTVLD_VP9_TILE_STATE)calloc(dwThreadNumber, sizeof(*pTileState));
        pFrameState->pTileStateBase     = pTileState;


        // Create Context Model
        pFrameState->FrameInfo.pContext = &pVp9HostVld->Context.CurrContext;
        pFrameState->pVp9HostVld        = pVp9HostVld;
        pFrameState->dwLastTaskID       = -1; // Invalid ID
        pFrameState++;
    }

    pContext                                        = &pVp9HostVld->Context.CurrContext;
    pContext->TxProbTables[TX_8X8].pui8ProbTable    = &pContext->TxProbTableSet.Tx_8X8[0][0];
    pContext->TxProbTables[TX_8X8].uiStride         = TX_8X8;
    pContext->TxProbTables[TX_16X16].pui8ProbTable  = &pContext->TxProbTableSet.Tx_16X16[0][0];
    pContext->TxProbTables[TX_16X16].uiStride       = TX_16X16;
    pContext->TxProbTables[TX_32X32].pui8ProbTable  = &pContext->TxProbTableSet.Tx_32X32[0][0];
    pContext->TxProbTables[TX_32X32].uiStride       = TX_32X32;

  
finish:
    return eStatus;
}

VAStatus Intel_HostvldVp9_QueryBufferSize (
    INTEL_HOSTVLD_VP9_HANDLE         hHostVld,
    uint32_t                             *pdwBufferSize)
{
    PINTEL_HOSTVLD_VP9_STATE pVp9HostVld = NULL;
    VAStatus                  eStatus     = VA_STATUS_SUCCESS;

    pVp9HostVld = (PINTEL_HOSTVLD_VP9_STATE)hHostVld;

    if (pdwBufferSize)
    {
        *pdwBufferSize = pVp9HostVld->dwBufferNumber;
    }

finish:
    return eStatus;
}

VAStatus Intel_HostvldVp9_SetOutputBuffer (
    INTEL_HOSTVLD_VP9_HANDLE         hHostVld,
    PINTEL_HOSTVLD_VP9_OUTPUT_BUFFER pOutputBuffer)
{
    PINTEL_HOSTVLD_VP9_STATE pVp9HostVld = NULL;
    VAStatus                  eStatus     = VA_STATUS_SUCCESS;



    pVp9HostVld = (PINTEL_HOSTVLD_VP9_STATE)hHostVld;

    pVp9HostVld->pOutputBufferBase = pOutputBuffer;

    return eStatus;
}

VAStatus Intel_HostvldVp9_Initialize (
    INTEL_HOSTVLD_VP9_HANDLE         hHostVld,
    PINTEL_HOSTVLD_VP9_VIDEO_BUFFER  pVideoBuffer)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld = NULL;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;


    pVp9HostVld = (PINTEL_HOSTVLD_VP9_STATE)hHostVld;

    {
        PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState = NULL;
        DWORD                               dwCurrIndex;

        dwCurrIndex = (pVp9HostVld->dwCurrIndex + 1) % pVp9HostVld->dwBufferNumber;
        pFrameState = pVp9HostVld->pFrameStateBase + dwCurrIndex;

        pFrameState->dwPrevIndex    = pVp9HostVld->dwCurrIndex;
        pFrameState->dwCurrIndex    = dwCurrIndex;
        pFrameState->pOutputBuffer  = pVp9HostVld->pOutputBufferBase + dwCurrIndex;
        pFrameState->pVideoBuffer   = pVideoBuffer;

        pVp9HostVld->dwCurrIndex    = dwCurrIndex;
    }

    return eStatus;
}

VAStatus Intel_HostvldVp9_PreParser (PVOID pVp9FrameState)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld     = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState     = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pPrevFrameState = NULL;
    PINTEL_HOSTVLD_VP9_TILE_STATE    pTileState      = NULL;
    PINTEL_HOSTVLD_VP9_VIDEO_BUFFER  pVideoBuffer;
    PINTEL_HOSTVLD_VP9_OUTPUT_BUFFER pOutputBuffer;
    PINTEL_HOSTVLD_VP9_FRAME_INFO    pFrameInfo;
    PINTEL_VP9_PIC_PARAMS            pPicParams;
    PINTEL_VP9_SEGMENT_PARAMS        pSegmentData;
    INT                                 iMarkerBit;
    DWORD                               dwNumAboveCtx, dwSize, i;
    DWORD                               dwPrevPicWidth, dwPrevPicHeight;
    BOOL                                bPrevShowFrame;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;



    pFrameState     = (PINTEL_HOSTVLD_VP9_FRAME_STATE)pVp9FrameState;
    pVp9HostVld     = pFrameState->pVp9HostVld;
    pVideoBuffer    = pFrameState->pVideoBuffer;
    pOutputBuffer   = pFrameState->pOutputBuffer;
    pFrameInfo      = &pFrameState->FrameInfo;

    pPrevFrameState = pFrameState->pVp9HostVld->pFrameStateBase + pFrameState->dwPrevIndex;
    dwPrevPicWidth  = pPrevFrameState->FrameInfo.dwPicWidthCropped;
    dwPrevPicHeight = pPrevFrameState->FrameInfo.dwPicHeightCropped;
    bPrevShowFrame  = pPrevFrameState->FrameInfo.bShowFrame;

    pPicParams                      = pVideoBuffer->pVp9PicParams;
    pSegmentData                    = pVideoBuffer->pVp9SegmentData;
    pFrameInfo->pPicParams          = pPicParams;
    pFrameInfo->pSegmentData        = pVideoBuffer->pVp9SegmentData;
    
    pFrameInfo->ui8SegEnabled       = pPicParams->PicFlags.fields.segmentation_enabled;
    pFrameInfo->ui8SegUpdMap        = pPicParams->PicFlags.fields.segmentation_update_map;
    pFrameInfo->ui8TemporalUpd      = pPicParams->PicFlags.fields.segmentation_temporal_update;
    
    pFrameInfo->LastFrameType       = pVp9HostVld->LastFrameType;
    pFrameInfo->dwPicWidthCropped   = pPicParams->FrameWidthMinus1 + 1;
    pFrameInfo->dwPicHeightCropped  = pPicParams->FrameHeightMinus1 + 1;
    pFrameInfo->dwPicWidth          = ALIGN(pPicParams->FrameWidthMinus1 + 1, 8);
    pFrameInfo->dwPicHeight         = ALIGN(pPicParams->FrameHeightMinus1 + 1, 8);
    pFrameInfo->dwB8Columns         = pFrameInfo->dwPicWidth >> VP9_LOG2_B8_SIZE;
    pFrameInfo->dwB8Rows            = pFrameInfo->dwPicHeight >> VP9_LOG2_B8_SIZE;
    pFrameInfo->dwB8ColumnsAligned  = ALIGN(pFrameInfo->dwB8Columns, VP9_B64_SIZE_IN_B8);
    pFrameInfo->dwB8RowsAligned     = ALIGN(pFrameInfo->dwB8Rows, VP9_B64_SIZE_IN_B8);
    pFrameInfo->dwPicWidthAligned   = pFrameInfo->dwB8ColumnsAligned << VP9_LOG2_B8_SIZE;
    pFrameInfo->dwLog2TileRows      = pPicParams->log2_tile_rows;
    pFrameInfo->dwLog2TileColumns   = pPicParams->log2_tile_columns;
    pFrameInfo->dwTileRows          = 1 << pPicParams->log2_tile_rows;
    pFrameInfo->dwTileColumns       = 1 << pPicParams->log2_tile_columns;
    pFrameInfo->dwMbStride          = pFrameInfo->dwB8ColumnsAligned;
    pFrameInfo->bLossLess           = pPicParams->PicFlags.fields.LosslessFlag;
    pFrameInfo->bShowFrame          = pPicParams->PicFlags.fields.show_frame;
    pFrameInfo->bIsIntraOnly        = 
        (pPicParams->PicFlags.fields.frame_type == KEY_FRAME) || 
        (pPicParams->PicFlags.fields.intra_only);
    pFrameInfo->bFrameParallelDisabled   = 
        !pPicParams->PicFlags.fields.frame_parallel_decoding_mode;
    pFrameInfo->bErrorResilientMode     = 
        pPicParams->PicFlags.fields.error_resilient_mode;
    pFrameInfo->uiFrameContextIndex     = 
        pPicParams->PicFlags.fields.frame_context_idx;
    pFrameInfo->uiResetFrameContext     = 
        pPicParams->PicFlags.fields.reset_frame_context;
    pFrameInfo->bIsKeyFrame         = pPicParams->PicFlags.fields.frame_type == KEY_FRAME;
    pFrameInfo->eInterpolationType      = 
        (INTEL_HOSTVLD_VP9_INTERPOLATION_TYPE)pPicParams->PicFlags.fields.mcomp_filter_type;
    
    //Inter
    pFrameInfo->bIsSwitchableInterpolation  = 
        pPicParams->PicFlags.fields.mcomp_filter_type == VP9_INTERP_SWITCHABLE;
    pFrameInfo->bAllowHighPrecisionMv                   = pPicParams->PicFlags.fields.allow_high_precision_mv;
    pFrameInfo->RefFrameSignBias[VP9_REF_FRAME_LAST]    = pPicParams->PicFlags.fields.LastRefSignBias;
    pFrameInfo->RefFrameSignBias[VP9_REF_FRAME_GOLDEN]  = pPicParams->PicFlags.fields.GoldenRefSignBias;
    pFrameInfo->RefFrameSignBias[VP9_REF_FRAME_ALTREF]  = pPicParams->PicFlags.fields.AltRefSignBias;
    
    for (i = 0; i < VP9_MAX_SEGMENTS; i++)
    {
        // pack segment QP
        pFrameInfo->SegQP[i][INTEL_HOSTVLD_VP9_YUV_PLANE_Y]  = 
            (((UINT32)pSegmentData->SegData[i].LumaACQuantScale) << 16) | pSegmentData->SegData[i].LumaDCQuantScale;
        pFrameInfo->SegQP[i][INTEL_HOSTVLD_VP9_YUV_PLANE_UV] = 
            (((UINT32)pSegmentData->SegData[i].ChromaACQuantScale) << 16) | pSegmentData->SegData[i].ChromaDCQuantScale;
    }
    
    Intel_HostvldVp9_GetPartitions(pFrameInfo, pVideoBuffer, pPicParams);
    
    // Initialize BAC engine
    iMarkerBit = Intel_HostvldVp9_BacEngineInit(
        &pFrameState->BacEngine, 
        pFrameInfo->FirstPartition.pu8Buffer, 
        pFrameInfo->FirstPartition.dwSize);
    
    if (0 != iMarkerBit)
    {
        eStatus = VA_STATUS_ERROR_OPERATION_FAILED;
        goto finish;
    }
    
    pFrameInfo->bResetContext = FALSE;
    if (pFrameInfo->bIsIntraOnly || pFrameInfo->bErrorResilientMode)
    {
        // reset context
        Intel_HostvldVp9_ResetContext(
            &pVp9HostVld->Context,
            pFrameInfo);
    }
    
    if (!pFrameInfo->bResetContext)
    {
        // If we didn't reset the context, initialize current frame context by copying from context table
        Intel_HostvldVp9_GetCurrFrameContext(
            &pVp9HostVld->Context, 
            pFrameInfo);
    }
    
        Intel_HostvldVp9_SetupSegmentationProbs(
            &pVp9HostVld->Context.CurrContext,
            pPicParams->SegTreeProbs,
            pPicParams->SegPredProbs);
    
    pFrameState->dwTileStatesInUse = MIN(pFrameInfo->dwTileColumns, pVp9HostVld->dwThreadNumber);
    pTileState                     = pFrameState->pTileStateBase;
    for (i = 0; i < pFrameState->dwTileStatesInUse; i++)
    {
        memset(&(pTileState->Count), 0, sizeof(pTileState->Count));
        pTileState++;
    }
    
    // Only reallocate above context buffers when picture width becomes bigger           //Need To Check
    dwNumAboveCtx = pFrameInfo->dwPicWidthAligned >> VP9_LOG2_B8_SIZE;
    if (dwNumAboveCtx > pFrameInfo->dwNumAboveCtx)
    {
        pFrameInfo->dwNumAboveCtx = dwNumAboveCtx;
    
        // Partition context, per 8x8 block
        dwSize = dwNumAboveCtx * sizeof(*pFrameInfo->pSegContextAbove);
        VP9_REALLOCATE_ABOVE_CTX_BUFFER(pFrameInfo->pSegContextAbove, dwSize);
    
        // Context for predicted segment id, per 8x8 block
        dwSize = dwNumAboveCtx * sizeof(*pFrameInfo->pPredSegIdContextAbove);
        VP9_REALLOCATE_ABOVE_CTX_BUFFER(pFrameInfo->pPredSegIdContextAbove, dwSize);
    
        // Entropy context, per 4x4 block. Allocate once for all the planes
        dwSize = (dwNumAboveCtx << 1) * sizeof(*pFrameInfo->pEntropyContextAbove[VP9_CODED_YUV_PLANE_Y]);
        VP9_REALLOCATE_ABOVE_CTX_BUFFER(pFrameInfo->pEntropyContextAbove[VP9_CODED_YUV_PLANE_Y], dwSize * VP9_CODED_YUV_PLANES);
        pFrameInfo->pEntropyContextAbove[VP9_CODED_YUV_PLANE_U] =
            pFrameInfo->pEntropyContextAbove[VP9_CODED_YUV_PLANE_Y] + dwSize;
        pFrameInfo->pEntropyContextAbove[VP9_CODED_YUV_PLANE_V] =
            pFrameInfo->pEntropyContextAbove[VP9_CODED_YUV_PLANE_U] + dwSize;
    }

    // Zero last segment id buffer if resolution changed
    if ((pFrameInfo->dwPicWidthCropped  != dwPrevPicWidth) || 
        (pFrameInfo->dwPicHeightCropped != dwPrevPicHeight))
    {
        // Reallocate last segment id buffer if it's not big enough
        dwSize = pFrameInfo->dwB8ColumnsAligned * pFrameInfo->dwB8RowsAligned;
        if (dwSize > pVp9HostVld->LastSegmentIndex.dwSize)
        {
            // Per 8x8 block, UINT8
            VP9_REALLOCATE_HOSTVLD_1D_BUFFER_UINT8(&pVp9HostVld->LastSegmentIndex, dwSize);
        }
        else
        {
            memset(pVp9HostVld->LastSegmentIndex.pu8Buffer, 0, pVp9HostVld->LastSegmentIndex.dwSize);
        }
    }
    pFrameState->pLastSegIdBuf = &pVp9HostVld->LastSegmentIndex;
    
    if (pVp9HostVld->pfnSyncCb)
    {
        pVp9HostVld->pfnSyncCb(
            pVp9HostVld->pvStandardState, 
            pVideoBuffer, 
            pFrameState->dwCurrIndex, 
            pFrameState->dwPrevIndex);
    }
    
    pFrameInfo->bHasPrevFrame = 
        (pFrameInfo->dwPicWidthCropped == dwPrevPicWidth)   &&
        (pFrameInfo->dwPicHeightCropped == dwPrevPicHeight) &&
        !pFrameInfo->bErrorResilientMode                    &&
        !pFrameInfo->bIsIntraOnly                           &&
        bPrevShowFrame;
    
    if (pFrameInfo->bIsIntraOnly)
    {
        Intel_HostvldVp9_FillIntraFrameRefFrame(&pOutputBuffer->ReferenceFrame);
    }
    
    if (pFrameInfo->bIsIntraOnly || pFrameInfo->bErrorResilientMode)
    {
        memset(pFrameState->pLastSegIdBuf->pu8Buffer, 0, pFrameState->pLastSegIdBuf->dwSize);
    }

    Intel_HostvldVp9_ParseCompressedHeader(pFrameState);

    Intel_HostvldVp9_PreParseTiles(pFrameState);

finish:
    return eStatus;
}


VAStatus Intel_HostvldVp9_PostParser (PVOID pVp9FrameState)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_INFO    pFrameInfo  = NULL;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;


    pFrameState = (PINTEL_HOSTVLD_VP9_FRAME_STATE)pVp9FrameState;
    pVp9HostVld = pFrameState->pVp9HostVld;
    pFrameInfo  = &pFrameState->FrameInfo;

    pVp9HostVld->Context.pCurrCount = &pFrameState->pTileStateBase->Count;

    if (pVp9HostVld->dwThreadNumber > 1)
    {
        Intel_HostvldVp9_PostParseTiles(pFrameState);
    }
    
    Intel_HostvldVp9_AdaptProbabilities(&pVp9HostVld->Context, pFrameInfo);
    
    Intel_HostvldVp9_RefreshFrameContext(&pVp9HostVld->Context, pFrameInfo);

    pVp9HostVld->LastFrameType = 
        (INTEL_HOSTVLD_VP9_FRAME_TYPE)pFrameInfo->pPicParams->PicFlags.fields.frame_type;

    pFrameState->ReferenceFrame.pu16Buffer = pFrameState->pOutputBuffer->ReferenceFrame.pu16Buffer;
    pFrameState->ReferenceFrame.dwSize     = pFrameState->pOutputBuffer->ReferenceFrame.dwSize;

    return eStatus;
}

VAStatus Intel_HostvldVp9_TileColumnParser (PVOID pVp9TileState)
{
    PINTEL_HOSTVLD_VP9_TILE_STATE    pTileState  = NULL;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;


    pTileState     = (PINTEL_HOSTVLD_VP9_TILE_STATE)pVp9TileState;

    while(pTileState->dwCurrColIndex < pTileState->dwTileColumns)
    {
        Intel_HostvldVp9_ParseTileColumn(pTileState, pTileState->dwCurrColIndex);
        pTileState->dwCurrColIndex  += pTileState->dwTileStateNumber;
    }

    return eStatus;
}

VAStatus Intel_HostvldVp9_Parser (PVOID pVp9FrameState)
{
    VAStatus eStatus = VA_STATUS_SUCCESS;


    eStatus = Intel_HostvldVp9_PreParser(pVp9FrameState);

    eStatus = Intel_HostvldVp9_ParseTiles((PINTEL_HOSTVLD_VP9_FRAME_STATE)pVp9FrameState);

    eStatus = Intel_HostvldVp9_PostParser(pVp9FrameState);

finish:
    return eStatus;
}


VAStatus Intel_HostvldVp9_LoopFilter (PVOID pVp9FrameState)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState = NULL;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;



    pFrameState = (PINTEL_HOSTVLD_VP9_FRAME_STATE)pVp9FrameState;
    pVp9HostVld = pFrameState->pVp9HostVld;

    Intel_HostvldVp9_LoopfilterFrame(pFrameState);

    if (pVp9HostVld->pfnDeblockCb)
    {
        eStatus = pVp9HostVld->pfnDeblockCb(
            pVp9HostVld->pvStandardState, 
            pFrameState->dwCurrIndex);
    }

finish:
    return eStatus;
}

VAStatus Intel_HostvldVp9_Render (PVOID pVp9FrameState)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_INFO    pFrameInfo;
    VASurfaceID                               ucCurrPicIndex,ucLastRefIdx,ucGoldenRefIdx,ucAltRefIdx;
    PINTEL_HOSTVLD_VP9_VIDEO_BUFFER  pVideoBuffer= NULL;
    PINTEL_VP9_PIC_PARAMS            pPicParams  = NULL;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;


    pFrameState     = (PINTEL_HOSTVLD_VP9_FRAME_STATE)pVp9FrameState;
    pVp9HostVld     = pFrameState->pVp9HostVld;
    pFrameInfo      = &pFrameState->FrameInfo;
    pVideoBuffer    = pFrameState->pVideoBuffer;
    pPicParams      = pVideoBuffer->pVp9PicParams;

    ucCurrPicIndex  = pPicParams->CurrPic;
    ucLastRefIdx    = pPicParams->RefFrameList[pPicParams->PicFlags.fields.LastRefIdx];
    ucGoldenRefIdx  = pPicParams->RefFrameList[pPicParams->PicFlags.fields.GoldenRefIdx];
    ucAltRefIdx     = pPicParams->RefFrameList[pPicParams->PicFlags.fields.AltRefIdx];
   
    if (pVp9HostVld->pfnRenderCb)
    {
        pVp9HostVld->pfnRenderCb(
            pVp9HostVld->pvStandardState, 
            pFrameState->dwCurrIndex, 
            pFrameState->dwPrevIndex);
    }

    return eStatus;
}

VAStatus Intel_HostvldVp9_Execute (
    INTEL_HOSTVLD_VP9_HANDLE         hHostVld)
{
    PINTEL_HOSTVLD_VP9_STATE         pVp9HostVld = NULL;
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState = NULL;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;
    PINTEL_HOSTVLD_VP9_VIDEO_BUFFER pVp9VideoBuffer = NULL;


    pVp9HostVld     = (PINTEL_HOSTVLD_VP9_STATE)hHostVld;

    pFrameState = pVp9HostVld->pFrameStateBase + pVp9HostVld->dwCurrIndex;

    eStatus = Intel_HostvldVp9_Parser(pFrameState);
    if (eStatus != VA_STATUS_SUCCESS)
	goto finish;

    eStatus = Intel_HostvldVp9_LoopFilter(pFrameState);
    if (eStatus != VA_STATUS_SUCCESS)
	goto finish;

    pVp9VideoBuffer = pFrameState->pVideoBuffer;

    if (pVp9VideoBuffer->slice_data_bo) {
        dri_bo_unmap(pVp9VideoBuffer->slice_data_bo);
	pVp9VideoBuffer->slice_data_bo = NULL;
    }

    eStatus = Intel_HostvldVp9_Render(pFrameState);
    if (eStatus != VA_STATUS_SUCCESS)
	goto finish;

finish:
    return eStatus;
}


VAStatus Intel_HostvldVp9_InitFrameState (
    PVOID                      pInitData,
    PVOID                      pData)
{
    PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState;
    PINTEL_HOSTVLD_VP9_TASK_USERDATA pTaskUserData;
    VAStatus                          eStatus     = VA_STATUS_SUCCESS;


    pFrameState     = (PINTEL_HOSTVLD_VP9_FRAME_STATE)pData;
    pTaskUserData   = (PINTEL_HOSTVLD_VP9_TASK_USERDATA)pInitData;

    pFrameState->pVideoBuffer  = pTaskUserData->pVideoBuffer;
    pFrameState->pOutputBuffer = pTaskUserData->pOutputBuffer;
    pFrameState->pRenderTarget = pTaskUserData->pVideoBuffer->pRenderTarget;
    pFrameState->dwCurrIndex   = pTaskUserData->dwCurrIndex;
    pFrameState->dwPrevIndex   = pTaskUserData->dwPrevIndex;

    return eStatus;
}


VAStatus Intel_HostvldVp9_Destroy (
    INTEL_HOSTVLD_VP9_HANDLE         hHostVld)
{
    PINTEL_HOSTVLD_VP9_STATE pVp9HostVld = NULL;
    VAStatus                  eStatus     = VA_STATUS_SUCCESS;
    unsigned int                        i           = 0;



    pVp9HostVld = (PINTEL_HOSTVLD_VP9_STATE)hHostVld;

    if (pVp9HostVld)
    {
        PINTEL_HOSTVLD_VP9_VIDEO_BUFFER  pVideoBuffer;
        PINTEL_HOSTVLD_VP9_FRAME_STATE   pFrameState;


        pFrameState = pVp9HostVld->pFrameStateBase;
        if (pFrameState)
        {
            for(i = 0; i < pVp9HostVld->dwBufferNumber; i++)
            {
                if (pFrameState)
                {
                    VP9_SafeFreeMemory(pFrameState->FrameInfo.pSegContextAbove);
                    VP9_SafeFreeMemory(pFrameState->FrameInfo.pPredSegIdContextAbove);
                    VP9_SafeFreeMemory(pFrameState->FrameInfo.pEntropyContextAbove[VP9_CODED_YUV_PLANE_Y]);
                    VP9_SafeFreeMemory(pFrameState->pTileStateBase);
                }
                pFrameState++;
            }
            
            VP9_SafeFreeMemory(pVp9HostVld->pFrameStateBase);
        }

        VP9_SafeFreeMemory(pVp9HostVld->LastSegmentIndex.pu8Buffer);
        pthread_mutex_destroy(&pVp9HostVld->MutexSync);

        free(pVp9HostVld);
    }

finish:
    return eStatus;
}