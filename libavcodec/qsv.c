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
#if HAVE_PTHREADS
#include <pthread.h>
#elif HAVE_W32THREADS
#include "w32pthreads.h"
#endif

#include "libavutil/time.h"
#include "avcodec.h"
#include "internal.h"
#include "qsv.h"

#if HAVE_THREADS
// atomic ops
#if defined (__GNUC__)
#define ff_qsv_atomic_inc(ptr) __sync_add_and_fetch(ptr, 1)
#define ff_qsv_atomic_dec(ptr) __sync_sub_and_fetch(ptr, 1)
#elif HAVE_WINDOWS_H
#include <windows.h>
#define ff_qsv_atomic_inc(ptr) InterlockedIncrement(ptr)
#define ff_qsv_atomic_dec(ptr) InterlockedDecrement(ptr)
#endif
#else
#define ff_qsv_atomic_inc(ptr) (*(ptr)++)
#define ff_qsv_atomic_dec(ptr) (*(ptr)--)
#endif

#if HAVE_THREADS
#define QSV_MUTEX_LOCK(x)                       \
    if (x)                                      \
        pthread_mutex_lock(x)
#define QSV_MUTEX_LOCK_COND(x, cond)            \
    if ((cond) && (x))                          \
        pthread_mutex_lock(x)
#define QSV_MUTEX_UNLOCK(x)                     \
    if (x)                                      \
        pthread_mutex_unlock(x)
#define QSV_MUTEX_UNLOCK_COND(x, cond)          \
    if ((cond) && (x))                          \
        pthread_mutex_unlock(x)
#define QSV_MUTEX_DESTROY(x)                    \
    if (x)                                      \
        pthread_mutex_destroy(&(x))

#else
#define QSV_MUTEX_LOCK(x)
#define QSV_MUTEX_LOCK_COND(x, cond)
#define QSV_MUTEX_UNLOCK(x)
#define QSV_MUTEX_UNLOCK_COND(x, cond)
#define QSV_MUTEX_DESTROY(x)
#endif

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
    l->mutex = NULL;
    return l;
}

int av_qsv_list_count(av_qsv_list *list)
{
    return list->items_count;
}

int av_qsv_list_add(av_qsv_list *list, void *elem)
{
    int pos;

    if (!elem)
        return -1;

    QSV_MUTEX_LOCK(list->mutex);
    if (list->items_count == list->items_alloc) {
        list->items_alloc += AV_QSV_JOB_SIZE_DEFAULT;
        list->items        = av_realloc(list->items,
                                        list->items_alloc * sizeof(void *));
        if (!list->items)
            return -1;
    }

    list->items[list->items_count] = elem;
    pos = list->items_count;
    list->items_count++;

    QSV_MUTEX_UNLOCK(list->mutex);

    return pos;
}

void av_qsv_list_del(av_qsv_list *list, void *elem)
{
    int i;

    QSV_MUTEX_LOCK(list->mutex);

    for (i = 0; i < list->items_count; i++)
        if (list->items[i] == elem) {
            memmove(&list->items[i], &list->items[i + 1],
                    (list->items_count - i - 1) * sizeof(void *));

            list->items_count--;
            break;
        }

    QSV_MUTEX_UNLOCK(list->mutex);
}

void *av_qsv_list_item(av_qsv_list *list, int pos)
{
    if (pos < 0 || pos >= list->items_count)
        return NULL;

    return list->items[pos];
}

void av_qsv_list_insert(av_qsv_list *list, int pos, void *elem)
{
    if (!elem)
        return;
    QSV_MUTEX_LOCK(list->mutex);

    if (list->items_count == list->items_alloc) {
        list->items_alloc += AV_QSV_JOB_SIZE_DEFAULT;
        list->items        = av_realloc(list->items,
                                        list->items_alloc * sizeof(void *));
        if (!list->items)
            return;
    }

    if (list->items_count != pos)
        memmove(&list->items[pos + 1], &list->items[pos],
                (list->items_count - pos) * sizeof(void *));

    list->items[pos] = elem;
    list->items_count++;

    QSV_MUTEX_UNLOCK(list->mutex);
}

void av_qsv_list_close(av_qsv_list *list)
{
    QSV_MUTEX_DESTROY(list->mutex);
    av_free(list->items);
    av_free(list);
    list = NULL;
}

static int qsv_is_sync_in_pipe(mfxSyncPoint *sync, av_qsv_context *qsv)
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
            if (sync == stage->out.p_sync)
                return 1;
        }
    }
    return 0;
}

static int qsv_is_surface_in_pipe(mfxFrameSurface1 *p_surface, av_qsv_context *qsv)
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

int av_qsv_get_free_sync(av_qsv_space *space, av_qsv_context *qsv)
{
    int i, counter = 0;

    while (1) {
        for (i = 0; i < space->sync_num; i++)
            if (!(*(space->p_sync[i])) &&
                !qsv_is_sync_in_pipe(space->p_sync[i], qsv)) {
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
    av_usleep(10000);
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
                !qsv_is_surface_in_pipe(space->p_surfaces[i], qsv)) {
                memcpy(&space->p_surfaces[i]->Info, info,
                       sizeof(space->p_surfaces[i]->Info));
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
    av_usleep(10000);
#endif
    }
    return -1;
}

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

// no duplicate of the same value, if end == 0 : working over full length
void av_qsv_dts_ordered_insert(av_qsv_context *qsv, int start, int end,
                               int64_t dts, int iter)
{
    av_qsv_dts *cur_dts, *new_dts;
    int i;

    QSV_MUTEX_LOCK_COND(!iter, qsv->qts_seq_mutex);

    if (!end)
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
    QSV_MUTEX_UNLOCK_COND(!iter, qsv->qts_seq_mutex);
}

void av_qsv_dts_pop(av_qsv_context *qsv)
{
    av_qsv_dts *item;

    QSV_MUTEX_LOCK_COND(qsv, qsv->qts_seq_mutex);

    if (av_qsv_list_count(qsv->dts_seq)) {
        item = av_qsv_list_item(qsv->dts_seq, 0);
        av_qsv_list_del(qsv->dts_seq, item);
        av_free(item);
    }
    QSV_MUTEX_UNLOCK_COND(qsv, qsv->qts_seq_mutex);
}

int av_qsv_context_clean(av_qsv_context *qsv)
{
    mfxStatus sts;
    int is_active = ff_qsv_atomic_dec(&qsv->is_context_active);

    if (is_active == 0) {
        if (qsv->dts_seq) {
            while (av_qsv_list_count(qsv->dts_seq))
                av_qsv_dts_pop(qsv);

            av_qsv_list_close(qsv->dts_seq);
        }
        QSV_MUTEX_DESTROY(qsv->qts_seq_mutex);
        qsv->qts_seq_mutex = 0;

        if (qsv->pipes)
            av_qsv_pipe_list_clean(qsv->pipes);

        if (qsv->mfx_session) {
            if ((sts = MFXClose(qsv->mfx_session)) < MFX_ERR_NONE)
                return sts;
            qsv->mfx_session = 0;
        }
    }
    return 0;
}

void av_qsv_add_context_usage(av_qsv_context *qsv, int is_threaded)
{
    int is_active = ff_qsv_atomic_inc(&qsv->is_context_active);

    if (is_active == 1) {
        memset(&qsv->mfx_session, 0, sizeof(qsv->mfx_session));
        av_qsv_pipe_list_create(qsv->pipes, is_threaded);

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

void av_qsv_pipe_list_create(av_qsv_list *list, int is_threaded)
{
    if (!list)
        list = av_qsv_list_init(is_threaded);
}

void av_qsv_pipe_list_clean(av_qsv_list *list)
{
    av_qsv_list *stage;
    int i;

    if (list) {
        for (i = av_qsv_list_count(list); i > 0; i--) {
            stage = av_qsv_list_item(list, i - 1);
            av_qsv_flush_stages(list, stage);
        }
        av_qsv_list_close(list);
    }
}

av_qsv_stage *av_qsv_stage_init(void)
{
    return av_mallocz(sizeof(av_qsv_stage));
}

void av_qsv_add_stage(av_qsv_list *list, av_qsv_stage *stage, int is_threaded)
{
    if (!list)
        list = av_qsv_list_init(is_threaded);
    else
        av_qsv_list_add(list, stage);
}

av_qsv_stage *av_qsv_get_last_stage(av_qsv_list *list)
{
    av_qsv_stage *stage;
    int size;

    QSV_MUTEX_LOCK(list);

    size = av_qsv_list_count(list);
    if (size > 0)
        stage = av_qsv_list_item(list, size - 1);

    QSV_MUTEX_LOCK(list);

    return stage;
}

void av_qsv_stage_clean(av_qsv_stage **stage)
{
    mfxSyncPoint *p_sync = (*stage)->out.p_sync;
    if (p_sync) {
        *p_sync = 0;
        p_sync  = NULL;
    }
    av_freep(stage);
}

void av_qsv_flush_stages(av_qsv_list *list, av_qsv_list *item)
{
    int i;
    av_qsv_stage **stage;

    for (i = 0; i < av_qsv_list_count(item); i++) {
        stage = av_qsv_list_item(item, i);
        av_qsv_stage_clean(stage);
    }
    av_qsv_list_del(list, item);
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

int av_is_qsv_available(mfxIMPL impl, mfxVersion *ver)
{
    mfxStatus sts;
    mfxSession mfx_session = { 0 };

    sts = MFXInit(impl, ver, &mfx_session);
    if (sts >= 0)
        MFXClose(mfx_session);
    return sts;
}
