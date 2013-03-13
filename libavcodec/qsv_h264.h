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
#ifndef AVCODEC_QSV_H264_H
#define AVCODEC_QSV_H264_H

#include "qsv.h"
#include "h264.h"

int ff_qsv_dec_init(AVCodecContext *avctx);
int ff_qsv_nal_find_start_code(uint8_t *pb, size_t size);

// Default allocators if SYSTEM MEMORY used
// as from MFXFrameAllocator
mfxStatus ff_qsv_mem_frame_alloc(mfxHDL pthis,
                                 mfxFrameAllocRequest *request,
                                 mfxFrameAllocResponse *response);
mfxStatus ff_qsv_mem_frame_lock(mfxHDL pthis, mfxMemId mid,
                                mfxFrameData *ptr);
mfxStatus ff_qsv_mem_frame_unlock(mfxHDL pthis, mfxMemId mid,
                                  mfxFrameData *ptr);
mfxStatus ff_qsv_mem_frame_getHDL(mfxHDL pthis, mfxMemId mid,
                                  mfxHDL *handle);
mfxStatus ff_qsv_mem_frame_free(mfxHDL pthis,
                                mfxFrameAllocResponse *response);
// as from mfxBufferAllocator
mfxStatus ff_qsv_mem_buffer_alloc(mfxHDL pthis, mfxU32 nbytes, mfxU16 type,
                                  mfxMemId *mid);
mfxStatus ff_qsv_mem_buffer_lock(mfxHDL pthis, mfxMemId mid, mfxU8 **ptr);
mfxStatus ff_qsv_mem_buffer_unlock(mfxHDL pthis, mfxMemId mid);
mfxStatus ff_qsv_mem_buffer_free(mfxHDL pthis, mfxMemId mid);

#endif /* AVCODEC_QSV_H264_H */
