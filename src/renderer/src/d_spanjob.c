/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 */
// d_spanjob.c -- the textured surfaces of D_DrawSurfaces() drawn on two CPUs
//
// The game thread works out each surface (its surface cache block, the
// gradients) and queues it; a worker thread and the game thread take them
// from the queue, build the surface cache block if it is new
// (R_DrawSurface(), whose state is each thread's own) and draw the spans
// (D_DrawSpans8Job(), D_DrawZSpansAt()).
// The spans of different surfaces never cover the same pixel, so the order
// they are drawn in does not change the picture. A batch ends with
// D_SpanJobsFinish() at the end of D_DrawSurfaces(), before the spans are
// used again. On the MiSTer the worker is on CPU1 (MiSTer_PinHelper()); it
// is not started if it cannot be, nor with MISTER_HYBRID_SPANTHREAD=0. The
// atomics are GCC's (the code is C99).


#include "d_local.h"
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef MISTER_HYBRID
#include "mister_port.h"
#endif

#define MAX_SPANJOBS 2048

static d_spanjob_t spanjobs[MAX_SPANJOBS];
static i32 queued; // jobs in the batch: written by the game thread only
static i32 next;   // the next job to take
static i32 done;   // jobs drawn
static i32 closed; // no more jobs come in this batch
static i32 busy;   // the worker is in a batch
static qboolean open; // a batch is open (the game thread's own)

static sem_t wake;
static sem_t started;
static i32 worker; // 0 not tried yet, 1 running, -1 off

static void D_RunSpanJob(const d_spanjob_t* job) {
    if (job->build) {
        // the game thread may be setting up a surface of its own
        // (D_CacheSurfaceFind() waiting for the jobs)
        const drawsurf_t own = r_drawsurf;
        r_drawsurf = job->drawsurf;
        R_DrawSurface();
        r_drawsurf = own;
    }
    D_DrawSpans8Job(job);
    D_DrawZSpansAt(job->spans, job->ziorigin, job->zistepu, job->zistepv);
    __atomic_fetch_sub(&job->cache->jobs, 1, __ATOMIC_RELEASE);
}

// Takes and draws one job; 0 if none is waiting
static qboolean D_TakeSpanJob(void) {
    i32 n = __atomic_load_n(&next, __ATOMIC_RELAXED);
    while (n < __atomic_load_n(&queued, __ATOMIC_ACQUIRE)) {
        if (__atomic_compare_exchange_n(&next, &n, n + 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            D_RunSpanJob(&spanjobs[n]);
            __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
            return true;
        }
    }
    return false;
}

// A batch is open from the first job queued until D_SpanJobsFinish()
void D_JobsOpen(void) {
    if (!open) {
        open = true;
        closed = 0;
        busy = 1;
        sem_post(&wake);
    }
}

static void* D_SpanWorker(void* arg) {
    i32 pinned = 1;
#ifdef MISTER_HYBRID
    pinned = MiSTer_PinHelper();
#endif
    worker = pinned ? 1 : -1;
    sem_post(&started);
    if (!pinned) {
        return NULL;
    }
    for (;;) {
        sem_wait(&wake);
        for (;;) {
            if (D_TakeSpanJob() || D_AliasTakeJob()) {
                continue;
            }
            if (__atomic_load_n(&closed, __ATOMIC_ACQUIRE)) {
                // a job queued before closed was set is seen now (alias
                // records the game thread may be drawing itself)
                if (!D_TakeSpanJob() && !D_AliasTakeJob()) {
                    break;
                }
                continue;
            }
            sched_yield();
        }
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

qboolean D_SpanJobsOn(void) {
    if (worker == 0) {
        const char* env = getenv("MISTER_HYBRID_SPANTHREAD");
        pthread_t thread;

        worker = -1;
        if (env && atoi(env) == 0) {
            return false;
        }
        sem_init(&wake, 0, 0);
        sem_init(&started, 0, 0);
        if (pthread_create(&thread, NULL, D_SpanWorker, NULL) != 0) {
            return false;
        }
        pthread_detach(thread);
        sem_wait(&started);
        printf("span worker %s\n", worker == 1 ? "on" : "off (no CPU1)");
    }
    return worker == 1;
}

void D_SpanJobQueue(const d_spanjob_t* job) {
    if (queued == MAX_SPANJOBS) {
        // a full queue: drawn at once
        __atomic_fetch_add(&job->cache->jobs, 1, __ATOMIC_RELAXED);
        D_RunSpanJob(job);
        return;
    }
    spanjobs[queued] = *job;
    __atomic_fetch_add(&job->cache->jobs, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&queued, queued + 1, __ATOMIC_RELEASE);
    D_JobsOpen();
}

void D_SpanJobsFinish(void) {
    if (!open) {
        return;
    }
    __atomic_store_n(&closed, 1, __ATOMIC_RELEASE);
    while (D_TakeSpanJob()) {
    }
    while (D_AliasJobsPending()) {
        D_AliasTakeJob();
    }
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != queued ||
           __atomic_load_n(&busy, __ATOMIC_ACQUIRE)) {
    }
    queued = 0;
    next = 0;
    done = 0;
    D_AliasJobsReset();
    open = false;
}

void D_SpanJobsRelease(surfcache_t* cache) {
    if (__atomic_load_n(&cache->jobs, __ATOMIC_ACQUIRE) > 0) {
        D_SpanJobsFinish();
    }
}
