#include "thread_pool.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORK_BUF_SIZE 65536

HANDLE* thread_pool_start(int count, worker_ctx_t* base_ctx, int* out_count) {
    *out_count = count;
    HANDLE* threads = (HANDLE*)calloc(count, sizeof(HANDLE));
    if (!threads) { *out_count = 0; return NULL; }

    for (int i = 0; i < count; i++) {
        worker_ctx_t* ctx = (worker_ctx_t*)malloc(sizeof(worker_ctx_t));
        if (!ctx) continue;
        memcpy(ctx, base_ctx, sizeof(worker_ctx_t));
        ctx->thread_id = i;

        threads[i] = CreateThread(NULL, 0, worker_thread_func, ctx, 0, NULL);
        if (!threads[i]) {
            warn("Failed to create worker thread %d", i);
            free(ctx);
        }
    }
    return threads;
}

void thread_pool_wait(HANDLE* threads, int count) {
    WaitForMultipleObjects(count, threads, TRUE, INFINITE);
}

void thread_pool_cleanup(HANDLE* threads, int count) {
    for (int i = 0; i < count; i++) {
        if (threads[i]) CloseHandle(threads[i]);
    }
    free(threads);
}

DWORD WINAPI worker_thread_func(LPVOID param) {
    worker_ctx_t* ctx = (worker_ctx_t*)param;
    int tid = ctx->thread_id;

    /* Allocate receive buffer */
    char* buf = (char*)malloc(WORK_BUF_SIZE);
    if (!buf) {
        warn("Worker %d: out of memory", tid);
        free(ctx);
        return 1;
    }

    segment_manager_t* mgr = ctx->segmgr;

    while (!*ctx->interrupted) {
        /* 1. Acquire a pending segment */
        segment_t* seg = segmgr_acquire_pending(mgr);
        if (!seg && segmgr_all_done(mgr)) break;

        if (!seg) {
            /* Try RollBack */
            seg = segmgr_try_steal(mgr, tid);
        }

        if (!seg) {
            /* Nothing to do - wait for new work or completion */
            if (segmgr_all_done(mgr)) break;

            EnterCriticalSection(&mgr->lock);
            if (mgr->pending_head > mgr->pending_tail && mgr->active_count == 0 && !segmgr_all_done(mgr)) {
                /* All remaining segments might be SUSPENDED - set them back to PENDING */
                for (int i = 0; i < mgr->segment_count; i++) {
                    if (mgr->segments[i].state == SEG_SUSPENDED &&
                        mgr->segments[i].suspend_until_ms <= GetTickCount64()) {
                        mgr->segments[i].state = SEG_PENDING;
                        mgr->pending_queue[++mgr->pending_tail] = i;
                    }
                }
                if (mgr->pending_head <= mgr->pending_tail) {
                    LeaveCriticalSection(&mgr->lock);
                    WakeConditionVariable(&mgr->cv_new_work);
                    continue;
                }
            }
            /* No SUSPENDED ready either - wait */
            BOOL ok = SleepConditionVariableCS(&mgr->cv_new_work, &mgr->lock, 1000);
            LeaveCriticalSection(&mgr->lock);
            if (!ok && GetLastError() == ERROR_TIMEOUT) continue;
            continue;
        }

        /* 2. Connect and send Range request */
        /* Build range strings */
        char range_start[32], range_end[32];
        _snprintf(range_start, sizeof(range_start), "%lld",
                  (long long)(seg->start_offset + seg->downloaded));
        _snprintf(range_end, sizeof(range_end), "%lld",
                  (long long)seg->end_offset);

        http_client_t http;
        if (http_connect(&http, ctx->url) != 0) {
            warn("Worker %d: connect failed - %s", tid, http.last_error);
            segmgr_error(mgr, seg);
            http_close(&http);
            continue;
        }

        char scheme[16], host[256], path[2048];
        int port;
        http_parse_url(ctx->url, scheme, sizeof(scheme),
                       host, sizeof(host), &port, path, sizeof(path));

        http_response_t resp;
        int r = http_request(&http, HTTP_GET, path,
                             range_start, range_end,
                             ctx->user_agent, ctx->referer,
                             ctx->extra_headers, ctx->extra_count,
                             &resp);
        trace("Worker %d: Range: bytes=%s-%s -> status=%d accept_ranges=%s",
              tid, range_start, range_end,
              resp.status_code, resp.accept_ranges ? "yes" : "no");
        if (r != 0) {
            warn("Worker %d: request error - %s", tid, http.last_error);
            segmgr_error(mgr, seg);
            http_close(&http);
            continue;
        }
        
        /* If server returns non-206, this segment request failed */
        if (resp.status_code != 206) {
            trace("Worker %d: non-206 response, suspending segment", tid);
            segmgr_error(mgr, seg);
            http_close(&http);
            continue;
        }

        seg->socket_fd = (int)http.fd;

        /* 3. Receive data loop */
        uint64_t last_speed_ms = GetTickCount64();
        int64_t chunk_bytes = 0;
        bool last_chunk = false;

        while (!*ctx->interrupted) {
            int bytes;
            if (resp.is_chunked) {
                bytes = http_read_body_chunked(&http, buf, WORK_BUF_SIZE, &last_chunk);
            } else {
                bytes = http_read_body(&http, buf, WORK_BUF_SIZE);
            }
            if (bytes < 0) {
                /* Error */
                segmgr_error(mgr, seg);
                break;
            }
            if (bytes == 0) {
                trace("Worker %d: EOF on body (seg downloaded=%lld of range %lld-%lld)", 
                      tid, (long long)seg->downloaded,
                      (long long)seg->start_offset, (long long)seg->end_offset);
                break;
            }

            /* Write at correct offset */
            int64_t write_offset = seg->start_offset + seg->downloaded;
            int written = file_write_at(ctx->output_file, write_offset, buf, bytes);
            if (written != bytes) {
                warn("Worker %d: write error at offset %lld", tid, (long long)write_offset);
                segmgr_error(mgr, seg);
                break;
            }

            seg->downloaded += bytes;
            InterlockedExchangeAdd64(ctx->global_downloaded, bytes);
            chunk_bytes += bytes;

            /* Update speed every 500ms */
            uint64_t now = GetTickCount64();
            if (now - last_speed_ms >= 500) {
                int64_t elapsed = now - last_speed_ms;
                if (elapsed > 0) {
                    EnterCriticalSection(ctx->speed_lock);
                    int64_t total = *ctx->global_downloaded;
                    /* Use a period-based speed calculation shared across threads */
                    static uint64_t prev_time = 0;
                    static int64_t prev_bytes = 0;
                    uint64_t dt = now - prev_time;
                    if (dt > 0 && prev_time > 0) {
                        *ctx->global_speed = (total - prev_bytes) * 1000 / (int64_t)dt;
                    }
                    prev_time = now;
                    prev_bytes = total;
                    LeaveCriticalSection(ctx->speed_lock);
                }
                seg->speed_bps = chunk_bytes * 1000 / (int64_t)(now - last_speed_ms + 1);
                chunk_bytes = 0;
                last_speed_ms = now;
                seg->last_activity_ms = now;
            }
        }

        http_close(&http);

        /* Check if segment completed successfully */
        EnterCriticalSection(&mgr->lock);
        bool complete = (seg->downloaded >= seg->end_offset - seg->start_offset + 1);
        LeaveCriticalSection(&mgr->lock);

        if (complete) {
            segmgr_complete(mgr, seg);
        }
        /* If error happened, seg state was already set by segmgr_error */
    }

    free(buf);
    free(ctx);
    return 0;
}
