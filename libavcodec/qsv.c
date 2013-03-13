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

#include "config.h"
#include "avcodec.h"
#include "internal.h"
#include "qsv.h"

#if HAVE_THREADS
// atomic ops
#if defined (__GNUC__)
#define ff_qsv_atomic_inc(ptr) __sync_add_and_fetch(ptr, 1)
#define ff_qsv_atomic_dec(ptr) __sync_sub_and_fetch(ptr, 1)
#elif HAVE_WINDOWS_H               // MSVC case
#include <windows.h>
#define ff_qsv_atomic_inc(ptr) InterlockedIncrement(ptr)
#define ff_qsv_atomic_dec(ptr) InterlockedDecrement(ptr)
#endif
// threading implementation
#if HAVE_PTHREADS
#include <pthread.h>
#elif HAVE_W32THREADS
#include "w32pthreads.h"
#endif
#else
#define ff_qsv_atomic_inc(ptr) ((*ptr)++)
#define ff_qsv_atomic_dec(ptr) ((*ptr)--)
#endif

int av_qsv_get_free_encode_task(av_qsv_list *tasks)
{
    int i;

    if (tasks)
        for (i = 0; i < av_qsv_list_count(tasks); i++) {
            av_qsv_task *task = av_qsv_list_item(tasks, i);
            if (task->stage && task->stage->out.p_sync)
                if (!(*task->stage->out.p_sync))
                    return i;
        }
    return MFX_ERR_NOT_FOUND;
}

int av_qsv_get_free_sync(av_qsv_space *space, av_qsv_context *qsv)
{
    int i, counter = 0;

    while (1) {
        for (i = 0; i < space->sync_num; i++)
            if (!(*(space->p_sync[i])) &&
                !ff_qsv_is_sync_in_pipe(space->p_sync[i], qsv)) {
                if (i > space->sync_num_max_used)
                    space->sync_num_max_used = i;
                return i;
            }

#if HAVE_THREADS
        if (++counter >= AV_QSV_REPEAT_NUM_DEFAULT) {
#endif
        av_log(NULL, AV_LOG_FATAL,
               "Not enough [%d] sync point(s) allocated.\n",
               space->sync_num);
        break;
#if HAVE_THREADS
    }
    av_qsv_sleep(10);
#endif
    }
    return -1;
}

int av_qsv_get_free_surface(av_qsv_space *space, av_qsv_context *qsv,
                            mfxFrameInfo *info, av_qsv_split part)
{
    int i, from, up, counter = 0;

    while (1) {
        from = 0;
        up   = space->surface_num;
        if (part == QSV_PART_LOWER)
            up /= 2;
        if (part == QSV_PART_UPPER)
            from = up / 2;

        for (i = from; i < up; i++)
            if ((!space->p_surfaces[i]->Data.Locked) &&
                !ff_qsv_is_surface_in_pipe(space->p_surfaces[i], qsv)) {
                memcpy(&(space->p_surfaces[i]->Info), info,
                       sizeof(mfxFrameInfo));
                if (i > space->surface_num_max_used)
                    space->surface_num_max_used = i;
                return i;
            }

#if HAVE_THREADS
        if (++counter >= AV_QSV_REPEAT_NUM_DEFAULT) {
#endif
        av_log(NULL, AV_LOG_FATAL,
               "Not enough [%d] surface(s) allocated.\n", up);
        break;
#if HAVE_THREADS
    }
    av_qsv_sleep(10);
#endif
    }
    return -1;
}

int ff_qsv_is_surface_in_pipe(mfxFrameSurface1 *p_surface, av_qsv_context *qsv)
{
    int a, b;
    av_qsv_list *list;
    av_qsv_stage *stage;

    if (!p_surface || !qsv->pipes)
        return 0;

    for (a = 0; a < av_qsv_list_count(qsv->pipes); a++) {
        list = av_qsv_list_item(qsv->pipes, a);
        for (b = 0; b < av_qsv_list_count(list); b++) {
            stage = av_qsv_list_item(list, b);
            if (p_surface == stage->out.p_surface ||
                p_surface == stage->in.p_surface)
                return 1;
        }
    }
    return 0;
}

int ff_qsv_is_sync_in_pipe(mfxSyncPoint *sync, av_qsv_context *qsv)
{
    int a, b;
    av_qsv_list *list;
    av_qsv_stage *stage;

    if (!sync || !qsv->pipes)
        return 0;

    for (a = 0; a < av_qsv_list_count(qsv->pipes); a++) {
        list = av_qsv_list_item(qsv->pipes, a);
        for (b = 0; b < av_qsv_list_count(list); b++) {
            stage = av_qsv_list_item(list, b);
            if (sync == stage->out.p_sync) {
                return 1;
            }
        }
    }
    return 0;
}

av_qsv_stage *av_qsv_stage_init(void)
{
    return av_mallocz(sizeof(av_qsv_stage));
}

void av_qsv_stage_clean(av_qsv_stage **stage)
{
    av_qsv_stage *stage_ptr = *stage;
    if (stage_ptr->out.p_sync) {
        *stage_ptr->out.p_sync = 0;
        stage_ptr->out.p_sync  = NULL;
    }
    av_freep(stage);
}

void av_qsv_add_context_usage(av_qsv_context *qsv, int is_threaded)
{
    int is_active = ff_qsv_atomic_inc(&qsv->is_context_active);

    if (is_active == 1) {
        AV_QSV_ZERO_MEMORY(qsv->mfx_session);
        av_qsv_pipe_list_create(&qsv->pipes, is_threaded);

        qsv->dts_seq = av_qsv_list_init(is_threaded);

#if HAVE_THREADS
        if (is_threaded) {
            qsv->qts_seq_mutex = av_mallocz(sizeof(pthread_mutex_t));
            if (qsv->qts_seq_mutex)
                pthread_mutex_init(qsv->qts_seq_mutex, NULL);
        } else
#endif
        qsv->qts_seq_mutex = 0;
    }
}

int av_qsv_context_clean(av_qsv_context *qsv)
{
    mfxStatus sts = MFX_ERR_NONE;
    int is_active = ff_qsv_atomic_dec(&qsv->is_context_active);

    if (is_active == 0) {
        if (qsv->dts_seq) {
            while (av_qsv_list_count(qsv->dts_seq))
                av_qsv_dts_pop(qsv);

            av_qsv_list_close(&qsv->dts_seq);
        }
#if HAVE_THREADS
        if (qsv->qts_seq_mutex)
            pthread_mutex_destroy(qsv->qts_seq_mutex);
#endif
        qsv->qts_seq_mutex = 0;

        if (qsv->pipes)
            av_qsv_pipe_list_clean(&qsv->pipes);

        if (qsv->mfx_session) {
            sts = MFXClose(qsv->mfx_session);
            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
            qsv->mfx_session = 0;
        }
    }
    return 0;
}

void av_qsv_pipe_list_create(av_qsv_list **list, int is_threaded)
{
    if (!*list)
        *list = av_qsv_list_init(is_threaded);
}

void av_qsv_pipe_list_clean(av_qsv_list **list)
{
    av_qsv_list *stage;
    int i;

    if (*list) {
        for (i = av_qsv_list_count(*list); i > 0; i--) {
            stage = av_qsv_list_item(*list, i - 1);
            av_qsv_flush_stages(*list, &stage);
        }
        av_qsv_list_close(list);
    }
}

void av_qsv_add_stage(av_qsv_list **list, av_qsv_stage *stage, int is_threaded)
{
    if (!*list)
        *list = av_qsv_list_init(is_threaded);
    if (*list)
        av_qsv_list_add(*list, stage);
}

av_qsv_stage *av_qsv_get_last_stage(av_qsv_list *list)
{
    av_qsv_stage *stage;
    int size;

#if HAVE_THREADS
    if (list->mutex)
        pthread_mutex_lock(list->mutex);
#endif

    size = av_qsv_list_count(list);
    if (size > 0)
        stage = av_qsv_list_item(list, size - 1);

#if HAVE_THREADS
    if (list->mutex)
        pthread_mutex_unlock(list->mutex);
#endif

    return stage;
}

void av_qsv_flush_stages(av_qsv_list *list, av_qsv_list **item)
{
    int i;
    av_qsv_stage *stage;

    for (i = 0; i < av_qsv_list_count(*item); i++) {
        stage = av_qsv_list_item(*item, i);
        av_qsv_stage_clean(&stage);
    }
    av_qsv_list_rem(list, *item);
    av_qsv_list_close(item);
}

av_qsv_list *av_qsv_pipe_by_stage(av_qsv_list *list, av_qsv_stage *stage)
{
    av_qsv_list *item;
    av_qsv_stage *cur_stage;
    int i, a;

    for (i = 0; i < av_qsv_list_count(list); i++) {
        item = av_qsv_list_item(list, i);
        for (a = 0; a < av_qsv_list_count(item); a++) {
            cur_stage = av_qsv_list_item(item, a);
            if (cur_stage == stage)
                return item;
        }
    }
    return 0;
}

// no duplicate of the same value, if end == 0 : working over full length
void av_qsv_dts_ordered_insert(av_qsv_context *qsv, int start, int end,
                               int64_t dts, int iter)
{
    av_qsv_dts *cur_dts, *new_dts;
    int i;

#if HAVE_THREADS
    if (iter == 0 && qsv->qts_seq_mutex)
        pthread_mutex_lock(qsv->qts_seq_mutex);
#endif

    if (end == 0)
        end = av_qsv_list_count(qsv->dts_seq);

    if (end <= start) {
        new_dts = av_mallocz(sizeof(*new_dts));
        if (new_dts) {
            new_dts->dts = dts;
            av_qsv_list_add(qsv->dts_seq, new_dts);
        }
    } else
        for (i = end; i > start; i--) {
            cur_dts = av_qsv_list_item(qsv->dts_seq, i - 1);
            if (cur_dts->dts < dts) {
                new_dts = av_mallocz(sizeof(*new_dts));
                if (new_dts) {
                    new_dts->dts = dts;
                    av_qsv_list_insert(qsv->dts_seq, i, new_dts);
                }
                break;
            } else if (cur_dts->dts == dts)
                break;
        }
#if HAVE_THREADS
    if (iter == 0 && qsv->qts_seq_mutex)
        pthread_mutex_unlock(qsv->qts_seq_mutex);
#endif
}

void av_qsv_dts_pop(av_qsv_context *qsv)
{
    av_qsv_dts *item;

#if HAVE_THREADS
    if (qsv && qsv->qts_seq_mutex)
        pthread_mutex_lock(qsv->qts_seq_mutex);
#endif

    if (av_qsv_list_count(qsv->dts_seq)) {
        item = av_qsv_list_item(qsv->dts_seq, 0);
        av_qsv_list_rem(qsv->dts_seq, item);
        av_free(item);
    }
#if HAVE_THREADS
    if (qsv && qsv->qts_seq_mutex)
        pthread_mutex_unlock(qsv->qts_seq_mutex);
#endif
}

av_qsv_list *av_qsv_list_init(int is_threaded)
{
    av_qsv_list *l = av_mallocz(sizeof(*l));

    if (!l)
        return NULL;
    l->items = av_mallocz_array(AV_QSV_JOB_SIZE_DEFAULT, sizeof(*l->items));
    if (!l->items) {
        av_free(l);
        return NULL;
    }
    l->items_alloc = AV_QSV_JOB_SIZE_DEFAULT;

#if HAVE_THREADS
    if (is_threaded) {
        l->mutex = av_mallocz(sizeof(pthread_mutex_t));
        if (!l->mutex) {
            av_free(l->items);
            av_free(l);
            return NULL;
        }
        if (l->mutex)
            pthread_mutex_init(l->mutex, NULL);
    } else
#endif
    l->mutex = 0;
    return l;
}

int av_qsv_list_count(av_qsv_list *list)
{
    return list->items_count;
}

int av_qsv_list_add(av_qsv_list *l, void *p)
{
    int pos;

    if (!p)
        return -1;

#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_lock(l->mutex);
#endif
    if (l->items_count == l->items_alloc) {
        l->items_alloc += AV_QSV_JOB_SIZE_DEFAULT;
        l->items        = av_realloc(l->items, l->items_alloc * sizeof(void *));
        if (!l->items)
            return -1;
    }

    l->items[l->items_count] = p;
    pos                      = l->items_count;
    l->items_count++;

#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_unlock(l->mutex);
#endif

    return pos;
}

void av_qsv_list_rem(av_qsv_list *l, void *p)
{
    int i;

#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_lock(l->mutex);
#endif

    for (i = 0; i < l->items_count; i++)
        if (l->items[i] == p) {
            memmove(&l->items[i], &l->items[i + 1],
                    (l->items_count - i - 1) * sizeof(void *));

            l->items_count--;
            break;
        }

#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_unlock(l->mutex);
#endif
}

void *av_qsv_list_item(av_qsv_list *l, int i)
{
    if (i < 0 || i >= l->items_count)
        return NULL;

    return l->items[i];
}

void av_qsv_list_insert(av_qsv_list *l, int pos, void *p)
{
    if (!p)
        return;
#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_lock(l->mutex);
#endif

    if (l->items_count == l->items_alloc) {
        l->items_alloc += AV_QSV_JOB_SIZE_DEFAULT;
        l->items        = av_realloc(l->items, l->items_alloc * sizeof(void *));
        if (!l->items)
            return;
    }

    if (l->items_count != pos) {
        memmove(&l->items[pos + 1], &l->items[pos],
                (l->items_count - pos) * sizeof(void *));
    }

    l->items[pos] = p;
    l->items_count++;

#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_unlock(l->mutex);
#endif
}

void av_qsv_list_close(av_qsv_list **list_close)
{
    av_qsv_list *l = *list_close;

#if HAVE_THREADS
    if (l->mutex)
        pthread_mutex_destroy(&l->mutex);
#endif

    av_free(l->items);
    av_free(l);

    *list_close = NULL;
}

int av_is_qsv_available(mfxIMPL impl, mfxVersion *ver)
{
    mfxStatus sts = MFX_ERR_NONE;
    mfxSession mfx_session = { 0 };

    sts = MFXInit(impl, ver, &mfx_session);
    if (sts >= 0)
        MFXClose(mfx_session);
    return sts;
}
