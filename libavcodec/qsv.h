/*
 * Copyright (C) 2013 Intel Corporation.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * - Neither the name of Intel Corporation nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY INTEL CORPORATION "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL INTEL CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef AVCODEC_QSV_H
#define AVCODEC_QSV_H

/**
 * @file
 * @ingroup lavc_codec_hwaccel_qsv
 * Common header for QSV/MediaSDK acceleration
 */

/**
 * @defgroup lavc_codec_hwaccel_qsv QSV/MediaSDK based Decode/Encode and Video Pre-Processing/VPP
 * @ingroup lavc_codec_hwaccel
 *
 *  As Intel Quick Sync Video (QSV) can decode/preprocess/encode with hardware
 *  acceleration
 *
 *  Supported features:
 *    - IO Pattern:
 *      - Opaque memory: MFX_IOPATTERN_OUT_OPAQUE_MEMORY // Video memory is
 *                       MFX_IMPL_HARDWARE or MFX_IMPL_AUTO and runtime support,
 *                       otherwise: System Memory
 *      - System memory: MFX_IOPATTERN_OUT_SYSTEM_MEMORY
 *    - Allocators:
 *      - default allocator for System memory: MFX_MEMTYPE_SYSTEM_MEMORY
 *    - details:
 *      implementation as "per frame"
 *
 *  TODO list:
 *      - format AV_PIX_FMT_QSV_MPEG2
 *      - format AV_PIX_FMT_QSV_VC1
 *    - IO Pattern:
 *      - VIDEO_MEMORY  // MFX_IOPATTERN_OUT_VIDEO_MEMORY
 *    - Allocators:
 *      - Video memory: MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET /
 *                      MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET
 *
 *  Note av_qsv_config struct required to fill in via
 *  AVCodecContext.hwaccel_context
 *
 *  As per frame, note AVFrame.data[2] (qsv_atom) used for frame atom id,
 *  data/linesize should be used together with SYSTEM_MEMORY and tested
 *
 *  Note: Compilation would require:
 *   - Intel MediaSDK headers, Full SDK is avaialble from the original web site:
 *                     http://software.intel.com/en-us/vcsource/tools/media-SDK
 *  and
 *  - Final application has to link against Intel MediaSDK dispatcher, available
 *     at MediaSDK as well
 *
 *  Target OS: as per available dispatcher and driver support
 *
 *  Implementation details:
 *   Provided struct av_qsv_context contain several struct av_qsv_space(s) for decode,
 *   VPP and encode.
 *   av_qsv_space just contain needed environment for the appropriate action.
 *   Based on this - pipeline (see pipes) will be build to pass details such as
 *   mfxFrameSurface1* and mfxSyncPoint* from one action to the next.
 *
 *  Resources re-usage (av_qsv_flush_stages), if resource_free || usage_threaded option:
 *     av_qsv_context *qsv = (av_qsv_context *)video_codec_ctx->priv_data;
 *     av_qsv_list *pipe = (av_qsv_list *)video_frame->data[2];
 *     av_qsv_flush_stages( qsv->pipes, &pipe );
 *
 *  DTS re-usage, if dts_need option:
 *     av_qsv_dts_pop(qsv);
 *
 *   for video,DX9/11 memory it has to be Unlock'ed as well
 *
 *  Implementation is thread aware and uses synchronization point(s) from MediaSDK
 *  as per configuration.
 *
 *  For the details of MediaSDK usage and options available - please refer to the
 *  available documentation at MediaSDK.
 *
 *  Feature set used from MSDK is defined by AV_QSV_MSDK_VERSION_MAJOR and
 *  AV_QSV_MSDK_VERSION_MINOR
 *
 * @{
 */

#include <stdint.h>
#include <string.h>
#include <mfx/mfxvideo.h>

#include "libavutil/log.h"

// sleep is defined in milliseconds
#define av_qsv_sleep(x) av_usleep((x) * 1000)

#define AV_QSV_ZERO_MEMORY(VAR)     { memset(&VAR, 0, sizeof(VAR)); }
#define AV_QSV_ALIGN32(X)           (((mfxU32)((X) + 31)) & (~(mfxU32)31))
#define AV_QSV_ALIGN16(value)       (((value + 15) >> 4) << 4)
#ifndef AV_QSV_PRINT_RET_MSG
#define AV_QSV_PRINT_RET_MSG(ERR)                                       \
    { av_log(NULL, AV_LOG_FATAL,                                        \
             "Error code %d,\t%s\t%d\n", ERR, __FUNCTION__, __LINE__);  \
    }
#endif

#ifndef AV_QSV_DEBUG_ASSERT
#define AV_QSV_DEBUG_ASSERT(x, y) { if ((x)) { av_log(NULL, AV_LOG_FATAL, "\nASSERT: %s\n", y); } }
#endif

#define AV_QSV_CHECK_RESULT(P, X, ERR)  { if ((X) > (P)) { AV_QSV_PRINT_RET_MSG(ERR); return ERR; } }
#define AV_QSV_CHECK_POINTER(P, ERR)    { if (!(P)) { AV_QSV_PRINT_RET_MSG(ERR); return ERR; } }
#define AV_QSV_IGNORE_MFX_STS(P, X)     { if ((X) == (P)) { P = MFX_ERR_NONE; } }

#define AV_QSV_ID_BUFFER MFX_MAKEFOURCC('B', 'U', 'F', 'F')
#define AV_QSV_ID_FRAME  MFX_MAKEFOURCC('F', 'R', 'M', 'E')

#define AV_QSV_SURFACE_NUM              80
#define AV_QSV_SYNC_NUM                 AV_QSV_SURFACE_NUM * 3 / 4
#define AV_QSV_BUF_SIZE_DEFAULT         4096 * 2160 * 10
#define AV_QSV_JOB_SIZE_DEFAULT         10
#define AV_QSV_SYNC_TIME_DEFAULT        10000
// see av_qsv_get_free_sync, av_qsv_get_free_surface , 100 if usleep(10*1000)(10ms) == 1 sec
#define AV_QSV_REPEAT_NUM_DEFAULT      100
#define AV_QSV_ASYNC_DEPTH_DEFAULT     4

// version of MSDK/QSV API currently used
#define AV_QSV_MSDK_VERSION_MAJOR  1
#define AV_QSV_MSDK_VERSION_MINOR  3

typedef enum AV_QSV_STAGE_TYPE {
#define AV_QSV_DECODE_MASK   0x001
    AV_QSV_DECODE = 0x001,

#define AV_QSV_VPP_MASK      0x0F0
    // "Mandatory VPP filter" , might be with "Hint-based VPP filters"
    AV_QSV_VPP_DEFAULT = 0x010,
    // "User Modules" etc
    AV_QSV_VPP_USER = 0x020,

#define av_QSV_ENCODE_MASK   0x100
    AV_QSV_ENCODE = 0x100
#define AV_QSV_ANY_MASK      0xFFF
} AV_QSV_STAGE_TYPE;

typedef struct av_qsv_stage {
    AV_QSV_STAGE_TYPE type;
    struct qsv_stage_in {
        mfxBitstream *p_bs;
        mfxFrameSurface1 *p_surface;
    } in;
    struct qsv_stage_out {
        mfxBitstream *p_bs;
        mfxFrameSurface1 *p_surface;
        mfxSyncPoint *p_sync;
    } out;
} av_qsv_stage;

typedef struct av_qsv_task {
    mfxBitstream *bs;
    av_qsv_stage *stage;
} av_qsv_task;

typedef struct av_qsv_list {
    // practically pthread_mutex_t
    void *mutex;

    void **items;
    int items_alloc;

    int items_count;
} av_qsv_list;

typedef struct av_qsv_space {
    uint8_t is_init_done;

    AV_QSV_STAGE_TYPE type;

    mfxVideoParam m_mfxVideoParam;

    mfxFrameAllocResponse response;
    mfxFrameAllocRequest request[2];    // [0] - in, [1] - out, if needed

    mfxExtOpaqueSurfaceAlloc ext_opaque_alloc;
    mfxExtBuffer *p_ext_params;

    uint16_t surface_num_max_used;
    uint16_t surface_num;
    mfxFrameSurface1 *p_surfaces[AV_QSV_SURFACE_NUM];

    uint16_t sync_num_max_used;
    uint16_t sync_num;
    mfxSyncPoint *p_sync[AV_QSV_SYNC_NUM];

    mfxBitstream bs;
    uint8_t *p_buf;
    size_t p_buf_max_size;

    // only for encode and tasks
    av_qsv_list *tasks;

    // storage for allocations/mfxMemId*
    mfxMemId *mids;
} av_qsv_space;

struct av_qsv_config;

typedef struct av_qsv_context {
    volatile int is_context_active;

    mfxIMPL impl;
    mfxSession mfx_session;
    mfxVersion ver;

    struct av_qsv_config *qsv_config;

    // decode
    av_qsv_space *dec_space;
    // encode
    av_qsv_space *enc_space;
    // vpp
    av_qsv_list *vpp_space;

    av_qsv_list *pipes;

    // MediaSDK starting from API version 1.6 includes DecodeTimeStamp
    // in addition to TimeStamp
    // see also AV_QSV_MSDK_VERSION_MINOR , AV_QSV_MSDK_VERSION_MAJOR
    av_qsv_list *dts_seq;

    // practically pthread_mutex_t
    void *qts_seq_mutex;
} av_qsv_context;

typedef enum av_qsv_split {
    QSV_PART_ANY = 0,
    QSV_PART_LOWER,
    QSV_PART_UPPER
} av_qsv_split;

typedef struct av_qsv_dts {
    int64_t dts;
} av_qsv_dts;

typedef struct av_qsv_alloc_frame {
    mfxU32 id;
    mfxFrameInfo info;
} av_qsv_alloc_frame;

typedef struct av_qsv_alloc_buffer {
    mfxU32 id;
    mfxU32 nbytes;
    mfxU16 type;
} av_qsv_alloc_buffer;

typedef struct av_qsv_allocators_space {
    av_qsv_space *space;
    mfxFrameAllocator frame_alloc;
    mfxBufferAllocator buffer_alloc;
} av_qsv_allocators_space;

typedef struct av_qsv_config {
    /**
     * Set async depth of processing with QSV / MSDK docs
     * Format: 0 and more
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int async_depth;

    /**
     * Range of numbers that indicate trade-offs between quality and speed.
     * Format: from 1/MFX_TARGETUSAGE_BEST_QUALITY to 7/MFX_TARGETUSAGE_BEST_SPEED inclusive
     *
     * - encoding: Set by user.
     * - decoding: unused
     */
    int target_usage;

    /**
     * Number of reference frames; if NumRefFrame = 0, this parameter is not specified.
     * Format: 0 and more
     *
     * - encoding: Set by user.
     * - decoding: unused
     */
    int num_ref_frame;

    /**
     * Distance between I- or P- key frames; if it is zero,
     * the GOP structure is unspecified.
     * Note: If GopRefDist = 1 no B-frames are used.
     *
     * - encoding: Set by user.
     * - decoding: unused
     */
    int gop_ref_dist;

    /**
     * Number of pictures within the current GOP (Group of Pictures);
     * if GopPicSize=0, then the GOP size is unspecified.
     * If GopPicSize=1, only I-frames are used.
     *
     * - encoding: Set by user.
     * - decoding: unused
     */
    int gop_pic_size;

    /**
     * Set type of surfaces used with QSV
     * Format: "IOPattern enum" of Media SDK
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int io_pattern;

    /**
     * Set amount of additional surfaces that might be needed.
     * Format: amount of additional buffers(surfaces+syncs)
     * to allocate in advance
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int additional_buffers;

    /**
     * If pipeline should be resource free.
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int resource_free;

    /**
     * If pipeline should be synchronous.
     * Format: wait time in milliseconds,
     *         AV_QSV_SYNC_TIME_DEFAULT/10000 might be a good value
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int sync_need;

    /**
     * If DTS workaround needed.
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int dts_need;

    /**
     * Type of implementation needed
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int impl_requested;

    /**
     * If QSV usage is multithreaded.
     * Format: Yes/No, 1/0
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int usage_threaded;

    /**
     * If QSV uses an external allocation (valid per session/mfxSession).
     * Format: pointer to allocators, if default: 0
     *
     * note that:
     * System Memory:   can be used without provided and external allocator,
     *  meaning MediaSDK will use an internal one
     * Video Memory:    in this case - we must provide an external allocator
     * Also, Media SDK session does not require external allocator if the
     *  application uses opaque memory
     *
     * Calls SetFrameAllocator/SetBufferAllocator
     * (MFXVideoCORE_SetFrameAllocator/MFXVideoCORE_SetBufferAllocator)
     * are to pass allocators to Media SDK
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    av_qsv_allocators_space *allocators;
} av_qsv_config;

static const uint8_t ff_prefix_code[] = { 0x00, 0x00, 0x00, 0x01 };

int av_qsv_get_free_sync(av_qsv_space *space, av_qsv_context *qsv);
int av_qsv_get_free_surface(av_qsv_space *space, av_qsv_context *qsv,
                            mfxFrameInfo *info, av_qsv_split part);
int av_qsv_get_free_encode_task(av_qsv_list *tasks);

int av_is_qsv_available(mfxIMPL impl, mfxVersion *ver);

void av_qsv_add_context_usage(av_qsv_context *qsv, int is_threaded);

void av_qsv_pipe_list_create(av_qsv_list **list, int is_threaded);
void av_qsv_pipe_list_clean(av_qsv_list **list);

void av_qsv_add_stagee(av_qsv_list **list, av_qsv_stage *stage,
                       int is_threaded);
av_qsv_stage *av_qsv_get_last_stage(av_qsv_list *list);
av_qsv_stage *av_qsv_get_by_mask(av_qsv_list *list, int mask,
                                 av_qsv_stage **prev, av_qsv_list **this_pipe);
av_qsv_list *av_qsv_pipe_by_stage(av_qsv_list *list, av_qsv_stage *stage);
void av_qsv_flush_stages(av_qsv_list *list, av_qsv_list **item);

void av_qsv_dts_ordered_insert(av_qsv_context *qsv, int start, int end,
                               int64_t dts, int iter);
void av_qsv_dts_pop(av_qsv_context *qsv);

av_qsv_stage *av_qsv_stage_init(void);
void av_qsv_stage_clean(av_qsv_stage **stage);
int av_qsv_context_clean(av_qsv_context *qsv);

int ff_qsv_is_sync_in_pipe(mfxSyncPoint *sync, av_qsv_context *qsv);
int ff_qsv_is_surface_in_pipe(mfxFrameSurface1 *p_surface, av_qsv_context *qsv);

av_qsv_list *av_qsv_list_init(int is_threaded);
int av_qsv_list_count(av_qsv_list *);
int av_qsv_list_add(av_qsv_list *, void *);
void av_qsv_list_rem(av_qsv_list *, void *);
void av_qsv_list_insert(av_qsv_list *, int, void *);
void *av_qsv_list_item(av_qsv_list *, int);
void av_qsv_list_close(av_qsv_list **);

/* @} */

#endif /* AVCODEC_QSV_H */
