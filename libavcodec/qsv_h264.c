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
 */

#include "qsv_h264.h"
#include "h264.h"

static av_qsv_config av_qsv_default_config = {
    .async_depth        = AV_QSV_ASYNC_DEPTH_DEFAULT,
    .target_usage       = MFX_TARGETUSAGE_BALANCED,
    .num_ref_frame      = 0,
    .gop_ref_dist       = 0,
    .gop_pic_size       = 0,
    .io_pattern         = MFX_IOPATTERN_OUT_SYSTEM_MEMORY,
    .additional_buffers = 0,
    .sync_need          = AV_QSV_SYNC_TIME_DEFAULT,
    .resource_free      = 1,
    .dts_need           = 0,
    .impl_requested     = MFX_IMPL_AUTO,
    .usage_threaded     = 0,
    .allocators         = 0,
};

static av_qsv_allocators_space av_qsv_default_system_allocators = {
    // fill to access mids
    .space        = 0,

    .frame_alloc  = {
        .pthis  = &av_qsv_default_system_allocators,
        .Alloc  = ff_qsv_mem_frame_alloc,
        .Lock   = ff_qsv_mem_frame_lock,
        .Unlock = ff_qsv_mem_frame_unlock,
        .GetHDL = ff_qsv_mem_frame_getHDL,
        .Free   = ff_qsv_mem_frame_free,
    },
    .buffer_alloc = {
        .pthis  = &av_qsv_default_system_allocators,
        .Alloc  = ff_qsv_mem_buffer_alloc,
        .Lock   = ff_qsv_mem_buffer_lock,
        .Unlock = ff_qsv_mem_buffer_unlock,
        .Free   = ff_qsv_mem_buffer_free,
    },
};

static const uint8_t qsv_prefix_code[] = { 0x00, 0x00, 0x00, 0x01 };
static const uint8_t qsv_slice_code[]  = { 0x00, 0x00, 0x01, 0x65 };

int ff_qsv_nal_find_start_code(uint8_t *pb, size_t size)
{
    if (size < 4)
        return 0;

    while ((size >= 4) && ((0 != pb[0]) || (0 != pb[1]) || (1 != pb[2]))) {
        pb   += 1;
        size -= 1;
    }

    if (size >= 4)
        return 1;

    return 0;
}

int ff_qsv_dec_init(AVCodecContext *avctx)
{
    int ret               = 0;
    mfxStatus sts         = MFX_ERR_NONE;
    size_t current_offset = 6;
    int header_size       = 0;
    int i                 = 0;
    unsigned char *current_position;
    size_t current_size;

    av_qsv_context *qsv               = avctx->priv_data;
    av_qsv_space *qsv_decode          = qsv->dec_space;
    av_qsv_config *qsv_config_context = avctx->hwaccel_context;

    qsv->impl = qsv_config_context->impl_requested;

    memset(&qsv->mfx_session, 0, sizeof(qsv->mfx_session));
    qsv->ver.Major = AV_QSV_MSDK_VERSION_MAJOR;
    qsv->ver.Minor = AV_QSV_MSDK_VERSION_MINOR;

    sts = MFXInit(qsv->impl, &qsv->ver, &qsv->mfx_session);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    qsv_decode->m_mfxVideoParam.mfx.CodecId = MFX_CODEC_AVC;
    qsv_decode->m_mfxVideoParam.IOPattern   = qsv_config_context->io_pattern;
    qsv_decode->m_mfxVideoParam.AsyncDepth  = qsv_config_context->async_depth;

    current_position = avctx->extradata;
    current_size     = avctx->extradata_size;

    if (!ff_qsv_nal_find_start_code(current_position, current_size)) {
        while (current_offset <= current_size) {
            int current_nal_size = current_position[current_offset] << 8 |
                                   current_position[current_offset + 1];
            unsigned char nal_type = current_position[current_offset + 2] & 0x1F;

            if (nal_type == NAL_SPS || nal_type == NAL_PPS) {
                memcpy(&qsv_decode->p_buf[header_size], qsv_prefix_code,
                       sizeof(qsv_prefix_code));
                header_size += sizeof(qsv_prefix_code);
                memcpy(&qsv_decode->p_buf[header_size],
                       &current_position[current_offset + 2],
                       current_nal_size);

                // fix for PPS as it comes after SPS, so - last
                if (nal_type == NAL_PPS) {
                    /* fix of MFXVideoDECODE_DecodeHeader: needs one SLICE
                     * to find, any SLICE */
                    memcpy(&qsv_decode->p_buf[header_size + current_nal_size],
                           qsv_slice_code, current_nal_size);
                    header_size += sizeof(qsv_slice_code);
                }
            }

            header_size    += current_nal_size;
            current_offset += current_nal_size + 3;
        }
    } else {
        memcpy(&qsv_decode->p_buf[0], avctx->extradata, avctx->extradata_size);
        header_size = avctx->extradata_size;
        memcpy(&qsv_decode->p_buf[header_size], qsv_slice_code,
               sizeof(qsv_slice_code));
        header_size += sizeof(qsv_slice_code);
    }

    qsv_decode->bs.Data       = qsv_decode->p_buf;
    qsv_decode->bs.DataLength = header_size;
    qsv_decode->bs.MaxLength  = qsv_decode->p_buf_max_size;

    if (qsv_decode->bs.DataLength > qsv_decode->bs.MaxLength) {
        av_log(avctx, AV_LOG_FATAL, "DataLength > MaxLength\n");
        return -1;
    }

    sts = MFXVideoDECODE_DecodeHeader(qsv->mfx_session, &qsv_decode->bs,
                                      &qsv_decode->m_mfxVideoParam);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    qsv_decode->bs.DataLength -= sizeof(qsv_slice_code);
    qsv_decode->bs.DataFlag    = MFX_BITSTREAM_COMPLETE_FRAME;

    memset(&qsv_decode->request, 0, sizeof(qsv_decode->request) * 2);
    sts = MFXVideoDECODE_QueryIOSurf(qsv->mfx_session,
                                     &qsv_decode->m_mfxVideoParam,
                                     qsv_decode->request);
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    qsv_decode->surface_num = FFMIN(qsv_decode->request[0].NumFrameSuggested +
                                    qsv_config_context->async_depth +
                                    qsv_config_context->additional_buffers,
                                    AV_QSV_SURFACE_NUM);
    if (qsv_decode->surface_num <= 0)
        qsv_decode->surface_num = AV_QSV_SURFACE_NUM;

    if (qsv_decode->m_mfxVideoParam.IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY) {
        // as per non-opaque memory:
        if (!qsv_config_context->allocators) {
            av_log(avctx, AV_LOG_INFO,
                   "Using default allocators for QSV decode\n");
            ((av_qsv_config *)avctx->hwaccel_context)->allocators =
                &av_qsv_default_system_allocators;
        }

        qsv_config_context->allocators->space = qsv_decode;

        qsv_decode->request[0].NumFrameMin       = qsv_decode->surface_num;
        qsv_decode->request[0].NumFrameSuggested = qsv_decode->surface_num;

        qsv_decode->request[0].Type  = MFX_MEMTYPE_EXTERNAL_FRAME |
                                       MFX_MEMTYPE_FROM_DECODE;
        qsv_decode->request[0].Type |= MFX_MEMTYPE_SYSTEM_MEMORY;

        qsv_config_context->allocators->frame_alloc.Alloc(qsv_config_context->allocators,
                                                          &qsv_decode->request[0],
                                                          &qsv_decode->response);
    }

    for (i = 0; i < qsv_decode->surface_num; i++) {
        qsv_decode->p_surfaces[i] = av_mallocz(sizeof(*qsv_decode->p_surfaces[i]));
        AV_QSV_CHECK_POINTER(qsv_decode->p_surfaces[i], AVERROR(ENOMEM));
        memcpy(&(qsv_decode->p_surfaces[i]->Info),
               &(qsv_decode->request[0].Info),
               sizeof(qsv_decode->p_surfaces[i]->Info));

        if (qsv_decode->m_mfxVideoParam.IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY) {
            sts = qsv_config_context->allocators->frame_alloc.Lock(qsv_config_context->allocators,
                                                                   qsv_decode->response.mids[i],
                                                                   &(qsv_decode->p_surfaces[i]->Data));
            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
        }
    }

    qsv_decode->sync_num = FFMIN(qsv_decode->surface_num, AV_QSV_SYNC_NUM);
    for (i = 0; i < qsv_decode->sync_num; i++) {
        qsv_decode->p_sync[i] = av_mallocz(sizeof(*qsv_decode->p_sync[i]));
        AV_QSV_CHECK_POINTER(qsv_decode->p_sync[i], AVERROR(ENOMEM));
    }

    memset(&qsv_decode->ext_opaque_alloc, 0, sizeof(qsv_decode->ext_opaque_alloc));

    if (qsv_decode->m_mfxVideoParam.IOPattern == MFX_IOPATTERN_OUT_OPAQUE_MEMORY) {
        qsv_decode->p_ext_params = (mfxExtBuffer *)&qsv_decode->ext_opaque_alloc;

        qsv_decode->ext_opaque_alloc.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
        qsv_decode->ext_opaque_alloc.Header.BufferSz = sizeof(qsv_decode->ext_opaque_alloc.Header.BufferSz);
        qsv_decode->ext_opaque_alloc.Out.Surfaces    = qsv_decode->p_surfaces;
        qsv_decode->ext_opaque_alloc.Out.NumSurface  = qsv_decode->surface_num;
        qsv_decode->ext_opaque_alloc.Out.Type        = qsv_decode->request[0].Type;

        qsv_decode->m_mfxVideoParam.ExtParam    = &qsv_decode->p_ext_params;
        qsv_decode->m_mfxVideoParam.NumExtParam = 1;
    }

    sts = MFXVideoDECODE_Init(qsv->mfx_session, &qsv_decode->m_mfxVideoParam);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    qsv_decode->is_init_done = 1;
    return ret;
}

static av_cold int qsv_decode_init(AVCodecContext *avctx)
{
    av_qsv_space *qsv_decode;
    av_qsv_context *qsv;
    av_qsv_config *qsv_config_context = avctx->hwaccel_context;

    if (!qsv_config_context) {
        av_log(avctx, AV_LOG_INFO,
               "Using default config for QSV decode\n");
        avctx->hwaccel_context = qsv_config_context = &av_qsv_default_config;
    } else {
        if (qsv_config_context->io_pattern != MFX_IOPATTERN_OUT_OPAQUE_MEMORY &&
            qsv_config_context->io_pattern != MFX_IOPATTERN_OUT_SYSTEM_MEMORY) {
            av_log_missing_feature(avctx, "MFX_IOPATTERN_OUT_SYSTEM_MEMORY type", 0);
            return AVERROR_PATCHWELCOME;
        }
    }

    qsv = av_mallocz(sizeof(*qsv));
    if (!qsv)
        return AVERROR(ENOMEM);

    qsv_decode = av_mallocz(sizeof(*qsv_decode));
    if (!qsv_decode) {
        av_free(qsv);
        return AVERROR(ENOMEM);
    }

    qsv_decode->p_buf_max_size = AV_QSV_BUF_SIZE_DEFAULT;
    qsv_decode->p_buf          = av_malloc_array(qsv_decode->p_buf_max_size,
                                                 sizeof(uint8_t));
    if (!qsv_decode->p_buf) {
        av_free(qsv_decode);
        av_free(qsv);
        return AVERROR(ENOMEM);
    }

    qsv->dec_space   = qsv_decode;
    avctx->priv_data = qsv;

    av_qsv_add_context_usage(qsv, HAVE_THREADS ?
                             qsv_config_context->usage_threaded :
                             HAVE_THREADS);

    // allocation of p_sync and p_surfaces inside of ff_qsv_dec_init
    return ff_qsv_dec_init(avctx);
}

static av_cold int qsv_decode_end(AVCodecContext *avctx)
{
    mfxStatus sts;
    av_qsv_context *qsv               = avctx->priv_data;
    av_qsv_config *qsv_config_context = avctx->hwaccel_context;
    int i;

    if (qsv) {
        av_qsv_space *qsv_decode = qsv->dec_space;
        if (qsv_decode && qsv_decode->is_init_done) {
            // todo: change to AV_LOG_INFO
            av_log(avctx, AV_LOG_QUIET,
                   "qsv_decode report done, max_surfaces: %u/%u , max_syncs: %u/%u\n",
                   qsv_decode->surface_num_max_used,
                   qsv_decode->surface_num,
                   qsv_decode->sync_num_max_used,
                   qsv_decode->sync_num);
        }

        if (qsv_config_context &&
            qsv_config_context->io_pattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY) {
            if (qsv_config_context->allocators) {
                sts = qsv_config_context->allocators->frame_alloc.Free(qsv_config_context->allocators,
                                                                       &qsv_decode->response);
                AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
            } else
                av_log(avctx, AV_LOG_FATAL,
                       "No QSV allocators found for cleanup\n");
        }
        // closing the own resources
        av_freep(&qsv_decode->p_buf);

        for (i = 0; i < qsv_decode->surface_num; i++)
            av_freep(&qsv_decode->p_surfaces[i]);

        qsv_decode->surface_num = 0;

        for (i = 0; i < qsv_decode->sync_num; i++)
            av_freep(&qsv_decode->p_sync[i]);

        qsv_decode->sync_num     = 0;
        qsv_decode->is_init_done = 0;

        av_freep(&qsv->dec_space);

        // closing common stuff
        av_qsv_context_clean(qsv);
    }

    return 0;
}

static int qsv_decode_frame(AVCodecContext *avctx, void *data,
                            int *data_size, AVPacket *avpkt)
{
    mfxStatus sts                     = MFX_ERR_NONE;
    av_qsv_context *qsv               = avctx->priv_data;
    av_qsv_space *qsv_decode          = qsv->dec_space;
    av_qsv_config *qsv_config_context = avctx->hwaccel_context;
    int *got_picture_ptr              = data_size;
    int ret_value                     = 1;
    uint8_t *current_position         = avpkt->data;
    int current_size                  = avpkt->size;
    int frame_processed               = 0;
    size_t frame_length               = 0;
    int surface_idx                   = 0;

    int sync_idx = 0;
    int current_nal_size;
    unsigned char nal_type;
    av_qsv_stage *new_stage;
    mfxBitstream *input_bs  = NULL;
    size_t current_offset   = 2;
    av_qsv_list *qsv_atom;
    av_qsv_list *pipe;

    AVFrame *picture = data;

    *got_picture_ptr = 0;

    if (qsv_decode->bs.DataOffset + qsv_decode->bs.DataLength + current_size >
        qsv_decode->bs.MaxLength) {
        memmove(&qsv_decode->bs.Data[0],
                qsv_decode->bs.Data + qsv_decode->bs.DataOffset,
                qsv_decode->bs.DataLength);
        qsv_decode->bs.DataOffset = 0;
    }

    if (current_size) {
        while (current_offset <= current_size) {
            current_nal_size = (current_position[current_offset - 2] << 24 |
                                current_position[current_offset - 1] << 16 |
                                current_position[current_offset]     <<  8 |
                                current_position[current_offset + 1]) - 1;
            nal_type = current_position[current_offset + 2] & 0x1F;

            frame_length += current_nal_size;

            memcpy(&qsv_decode->bs.Data[0] +
                   qsv_decode->bs.DataLength +
                   qsv_decode->bs.DataOffset, qsv_prefix_code,
                   sizeof(qsv_prefix_code));
            qsv_decode->bs.DataLength += sizeof(qsv_prefix_code);
            memcpy(&qsv_decode->bs.Data[0] +
                   qsv_decode->bs.DataLength +
                   qsv_decode->bs.DataOffset,
                   &current_position[current_offset + 2],
                   current_nal_size + 1);
            qsv_decode->bs.DataLength += current_nal_size + 1;

            current_offset += current_nal_size + 5;
        }

        if (qsv_decode->bs.DataLength > qsv_decode->bs.MaxLength) {
            av_log(avctx, AV_LOG_FATAL, "DataLength > MaxLength\n");
            return -1;
        }
    }

    if (frame_length || !current_size) {
        qsv_decode->bs.TimeStamp = avpkt->pts;

        if (qsv_config_context->dts_need) {
            //not a drain
            if (current_size || qsv_decode->bs.DataLength)
                av_qsv_dts_ordered_insert(qsv, 0, 0,
                                          qsv_decode->bs.TimeStamp, 0);
        }

        sts = MFX_ERR_NONE;
        // ignore warnings, where warnings >0 , and not error codes <0
        while (MFX_ERR_NONE         <= sts ||
               MFX_ERR_MORE_SURFACE == sts ||
               MFX_WRN_DEVICE_BUSY  == sts) {
            if (sts == MFX_ERR_MORE_SURFACE || sts == MFX_ERR_NONE) {
                surface_idx = av_qsv_get_free_surface(qsv_decode, qsv,
                                                      &qsv_decode->request[0].Info,
                                                      QSV_PART_ANY);
                if (surface_idx == -1) {
                    *got_picture_ptr = 0;
                    return 0;
                }
            }

            if (sts == MFX_WRN_DEVICE_BUSY)
                av_qsv_sleep(10);

            sync_idx = av_qsv_get_free_sync(qsv_decode, qsv);
            if (sync_idx == -1) {
                *got_picture_ptr = 0;
                return 0;
            }
            new_stage = av_qsv_stage_init();
            if (!new_stage) {
                av_log(avctx, AV_LOG_FATAL, "Could not allocate stage\n");
                return -1;
            }
            input_bs = NULL;
            // if to drain last ones
            if (current_size || qsv_decode->bs.DataLength)
                input_bs = &qsv_decode->bs;

            // Decode a frame asynchronously (returns immediately)
            // very first IDR / SLICE should be with SPS/PPS
            sts = MFXVideoDECODE_DecodeFrameAsync(qsv->mfx_session, input_bs,
                                                  qsv_decode->p_surfaces[surface_idx],
                                                  &new_stage->out.p_surface,
                                                  qsv_decode->p_sync[sync_idx]);

            if (sts <= MFX_ERR_NONE        &&
                sts != MFX_WRN_DEVICE_BUSY &&
                sts != MFX_WRN_VIDEO_PARAM_CHANGED) {
                new_stage->type         = AV_QSV_DECODE;
                new_stage->in.p_bs      = input_bs;
                new_stage->in.p_surface = qsv_decode->p_surfaces[surface_idx];

                /* see MFXVideoDECODE_DecodeFrameAsync, will be filled
                 * from there, if output */
                new_stage->out.p_sync = qsv_decode->p_sync[sync_idx];
                if (!qsv_config_context->resource_free ||
                    qsv_config_context->usage_threaded) {
                    pipe = av_qsv_list_init(HAVE_THREADS ?
                                            qsv_config_context->usage_threaded :
                                            HAVE_THREADS);
                    av_qsv_add_stage(&pipe, new_stage,
                                     HAVE_THREADS ?
                                     qsv_config_context->usage_threaded :
                                     HAVE_THREADS);

                    av_qsv_list_add(qsv->pipes, pipe);
                    qsv_atom = pipe;
                } else {
                    qsv_atom = NULL;
                }

                /* Usage for forced decode sync and results, can be avoided if
                 * sync done by next stage. Also note wait time for Sync and
                 * possible usage with MFX_WRN_IN_EXECUTION check. */
                if (qsv_config_context->sync_need) {
                    sts = MFXVideoCORE_SyncOperation(qsv->mfx_session,
                                                     *new_stage->out.p_sync,
                                                     qsv_config_context->sync_need);
                    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

                    // no need to wait more -> force off
                    *new_stage->out.p_sync = 0;
                    new_stage->out.p_sync  = NULL;
                }

                sts = MFX_ERR_NONE;
                break;
            }
            av_qsv_stage_clean(&new_stage);

            /* Can be because of:
             * - runtime situation:
             * - drain procedure:
             * At the end of the bitstream, the application continuously calls
             * the MFXVideoDECODE_DecodeFrameAsync function with a NULL
             * bitstream pointer to drain any remaining frames cached within
             * the Intel Media SDK decoder, until the function returns
             * MFX_ERR_MORE_DATA. */
            if (sts == MFX_ERR_MORE_DATA) {
                // not a drain
                if (current_size) {
                    *got_picture_ptr = 0;
                    return avpkt->size;
                }
                // drain
                break;
            }

            if (sts == MFX_ERR_MORE_SURFACE || sts == MFX_ERR_MORE_SURFACE)
                continue;

            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
        }

        frame_processed = 1;
    }

    if (frame_processed) {
        if (current_size) {
            *got_picture_ptr = 1;
            ret_value        = avpkt->size;
        } else {
            if (sts != MFX_ERR_MORE_DATA) {
                *got_picture_ptr = 1;
                ret_value        = avpkt->size;
            } else {
                *got_picture_ptr = 0;
                return 0;
            }
        }

        picture->pkt_pts = avpkt->pts;
        picture->pts     = avpkt->pts;

        picture->repeat_pict      = qsv_decode->m_mfxVideoParam.mfx.FrameInfo.PicStruct   & MFX_PICSTRUCT_FIELD_REPEATED;
        picture->top_field_first  = qsv_decode->m_mfxVideoParam.mfx.FrameInfo.PicStruct   & MFX_PICSTRUCT_FIELD_TFF;
        picture->interlaced_frame = !(qsv_decode->m_mfxVideoParam.mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);

        // since we do not know it yet from MSDK, let's do just a simple way for now
        picture->key_frame = (avctx->frame_number == 0) ? 1 : 0;

        if (qsv_decode->m_mfxVideoParam.IOPattern ==
            MFX_IOPATTERN_OUT_SYSTEM_MEMORY) {
            picture->data[0]     = new_stage->out.p_surface->Data.Y;
            picture->data[1]     = new_stage->out.p_surface->Data.VU;
            picture->linesize[0] = new_stage->out.p_surface->Info.Width;
            picture->linesize[1] = new_stage->out.p_surface->Info.Width;
        } else {
            picture->data[0]     = 0;
            picture->data[1]     = 0;
            picture->linesize[0] = 0;
            picture->linesize[1] = 0;
        }

        picture->data[2]     = qsv_atom;
        picture->linesize[2] = 0;

        // assuming resource_free approach
        if (!qsv_atom)
            av_qsv_stage_clean(&new_stage);
    }
    return ret_value;
}

static void qsv_flush_dpb(AVCodecContext *avctx)
{
    av_qsv_context *qsv      = avctx->priv_data;
    av_qsv_space *qsv_decode = qsv->dec_space;

    qsv_decode->bs.DataOffset = 0;
    qsv_decode->bs.DataLength = 0;
    qsv_decode->bs.MaxLength  = qsv_decode->p_buf_max_size;
}

mfxStatus ff_qsv_mem_frame_alloc(mfxHDL pthis,
                                 mfxFrameAllocRequest *request,
                                 mfxFrameAllocResponse *response)
{
    mfxStatus sts = MFX_ERR_NONE;

    mfxU32 numAllocated = 0;

    mfxU32 width  = AV_QSV_ALIGN32(request->Info.Width);
    mfxU32 height = AV_QSV_ALIGN32(request->Info.Height);
    mfxU32 nbytes;

    av_qsv_allocators_space *this_alloc = (av_qsv_allocators_space *)pthis;
    av_qsv_alloc_frame *fs;

    if (!this_alloc->space)
        return MFX_ERR_NOT_INITIALIZED;

    switch (request->Info.FourCC) {
    case MFX_FOURCC_YV12:
    case MFX_FOURCC_NV12:
        nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
        break;
    case MFX_FOURCC_RGB3:
        nbytes = width * height + width * height + width * height;
        break;
    case MFX_FOURCC_RGB4:
        nbytes = width * height + width * height + width * height + width * height;
        break;
    case MFX_FOURCC_YUY2:
        nbytes = width * height + (width >> 1) * height + (width >> 1) * height;
        break;
    default:
        return MFX_ERR_UNSUPPORTED;
    }

    this_alloc->space->mids = av_malloc_array(request->NumFrameSuggested,
                                              sizeof(*this_alloc->space->mids));
    if (!this_alloc->space->mids)
        return MFX_ERR_MEMORY_ALLOC;

    // allocate frames
    for (numAllocated = 0; numAllocated < request->NumFrameSuggested; numAllocated++) {
        sts = this_alloc->buffer_alloc.Alloc(this_alloc->buffer_alloc.pthis,
                                             nbytes + AV_QSV_ALIGN32(sizeof(av_qsv_alloc_frame)),
                                             request->Type,
                                             &this_alloc->space->mids[numAllocated]);

        if (sts != MFX_ERR_NONE)
            break;

        sts = this_alloc->buffer_alloc.Lock(this_alloc->buffer_alloc.pthis,
                                            this_alloc->space->mids[numAllocated],
                                            (mfxU8 **)&fs);
        if (sts != MFX_ERR_NONE)
            break;

        fs->id   = AV_QSV_ID_FRAME;
        fs->info = request->Info;
        this_alloc->buffer_alloc.Unlock(this_alloc->buffer_alloc.pthis,
                                        this_alloc->space->mids[numAllocated]);
    }

    // check the number of allocated frames
    if (numAllocated < request->NumFrameMin)
        return MFX_ERR_MEMORY_ALLOC;

    response->NumFrameActual = (mfxU16)numAllocated;
    response->mids           = this_alloc->space->mids;

    return MFX_ERR_NONE;
}

mfxStatus ff_qsv_mem_frame_lock(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
    mfxStatus sts;
    av_qsv_alloc_frame *fs;
    mfxU16 width, height;

    av_qsv_allocators_space *this_alloc = (av_qsv_allocators_space *)pthis;

    if (!this_alloc->space)
        return MFX_ERR_NOT_INITIALIZED;
    if (!ptr)
        return MFX_ERR_NULL_PTR;

    sts = this_alloc->buffer_alloc.Lock(this_alloc->buffer_alloc.pthis, mid, (mfxU8 **)&fs);

    if (sts != MFX_ERR_NONE)
        return sts;

    if (fs->id != AV_QSV_ID_FRAME) {
        this_alloc->buffer_alloc.Unlock(this_alloc->buffer_alloc.pthis, mid);
        return MFX_ERR_INVALID_HANDLE;
    }

    width  = (mfxU16)AV_QSV_ALIGN32(fs->info.Width);
    height = (mfxU16)AV_QSV_ALIGN32(fs->info.Height);
    ptr->B = ptr->Y = (mfxU8 *)fs + AV_QSV_ALIGN32(sizeof(av_qsv_allocators_space));

    switch (fs->info.FourCC) {
    case MFX_FOURCC_NV12:
        ptr->U     = ptr->Y + width * height;
        ptr->V     = ptr->U + 1;
        ptr->Pitch = width;
        break;
    case MFX_FOURCC_YV12:
        ptr->V     = ptr->Y + width * height;
        ptr->U     = ptr->V + (width >> 1) * (height >> 1);
        ptr->Pitch = width;
        break;
    case MFX_FOURCC_YUY2:
        ptr->U     = ptr->Y + 1;
        ptr->V     = ptr->Y + 3;
        ptr->Pitch = 2 * width;
        break;
    case MFX_FOURCC_RGB3:
        ptr->G     = ptr->B + 1;
        ptr->R     = ptr->B + 2;
        ptr->Pitch = 3 * width;
        break;
    case MFX_FOURCC_RGB4:
        ptr->G     = ptr->B + 1;
        ptr->R     = ptr->B + 2;
        ptr->A     = ptr->B + 3;
        ptr->Pitch = 4 * width;
        break;
    default:
        return MFX_ERR_UNSUPPORTED;
    }

    return MFX_ERR_NONE;
}

mfxStatus ff_qsv_mem_frame_unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
    av_qsv_allocators_space *this_alloc = (av_qsv_allocators_space *)pthis;
    mfxStatus sts = this_alloc->buffer_alloc.Unlock(this_alloc->buffer_alloc.pthis, mid);

    if (sts != MFX_ERR_NONE)
        return sts;

    if (ptr) {
        ptr->Pitch = 0;
        ptr->Y     = 0;
        ptr->U     = 0;
        ptr->V     = 0;
    }

    return MFX_ERR_NONE;
}

mfxStatus ff_qsv_mem_frame_getHDL(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus ff_qsv_mem_frame_free(mfxHDL pthis, mfxFrameAllocResponse *response)
{
    mfxStatus sts                       = MFX_ERR_NONE;
    av_qsv_allocators_space *this_alloc = (av_qsv_allocators_space *)pthis;
    int i;

    if (!response)
        return MFX_ERR_NULL_PTR;

    if (!this_alloc->space)
        return MFX_ERR_NOT_INITIALIZED;

    if (response->mids)
        for (i = 0; i < response->NumFrameActual; i++)
            if (response->mids[i]) {
                sts = this_alloc->buffer_alloc.Free(this_alloc->buffer_alloc.pthis,
                                                    response->mids[i]);
                if (sts != MFX_ERR_NONE)
                    return sts;
            }

    av_freep(&response->mids);

    return sts;
}

mfxStatus ff_qsv_mem_buffer_alloc(mfxHDL pthis, mfxU32 nbytes, mfxU16 type,
                                  mfxMemId *mid)
{
    av_qsv_alloc_buffer *bs;
    mfxU32 header_size;

    if (!mid)
        return MFX_ERR_NULL_PTR;

    if (!(type & MFX_MEMTYPE_SYSTEM_MEMORY))
        return MFX_ERR_UNSUPPORTED;

    header_size = AV_QSV_ALIGN32(sizeof(av_qsv_alloc_buffer));
    bs          = av_malloc(header_size + nbytes);

    if (!bs)
        return MFX_ERR_MEMORY_ALLOC;

    bs->id     = AV_QSV_ID_BUFFER;
    bs->type   = type;
    bs->nbytes = nbytes;
    *mid       = (mfxHDL)bs;

    return MFX_ERR_NONE;
}

mfxStatus ff_qsv_mem_buffer_lock(mfxHDL pthis, mfxMemId mid, mfxU8 **ptr)
{
    av_qsv_alloc_buffer *bs;

    if (!ptr)
        return MFX_ERR_NULL_PTR;

    bs = (av_qsv_alloc_buffer *)mid;

    if (!bs)
        return MFX_ERR_INVALID_HANDLE;
    if (bs->id != AV_QSV_ID_BUFFER)
        return MFX_ERR_INVALID_HANDLE;

    *ptr = (mfxU8 *)bs + AV_QSV_ALIGN32(sizeof(av_qsv_alloc_buffer));
    return MFX_ERR_NONE;
}

mfxStatus ff_qsv_mem_buffer_unlock(mfxHDL pthis, mfxMemId mid)
{
    av_qsv_alloc_buffer *bs = (av_qsv_alloc_buffer *)mid;

    if (!bs || bs->id != AV_QSV_ID_BUFFER)
        return MFX_ERR_INVALID_HANDLE;

    return MFX_ERR_NONE;
}

mfxStatus ff_qsv_mem_buffer_free(mfxHDL pthis, mfxMemId mid)
{
    av_qsv_alloc_buffer *bs = (av_qsv_alloc_buffer *)mid;
    if (!bs || AV_QSV_ID_BUFFER != bs->id)
        return MFX_ERR_INVALID_HANDLE;

    av_freep(&bs);
    return MFX_ERR_NONE;
}

AVCodec ff_h264_qsv_decoder = {
    .name         = "h264_qsv",
    .type         = AVMEDIA_TYPE_VIDEO,
    .id           = AV_CODEC_ID_H264,
    .init         = qsv_decode_init,
    .close        = qsv_decode_end,
    .decode       = qsv_decode_frame,
    .capabilities = CODEC_CAP_DELAY,
    .flush        = qsv_flush_dpb,
    .long_name    = NULL_IF_CONFIG_SMALL("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (Intel QSV acceleration)"),
    .pix_fmts     = (const enum PixelFormat[]) { AV_PIX_FMT_QSV_H264,
                                                 AV_PIX_FMT_NONE },
};
