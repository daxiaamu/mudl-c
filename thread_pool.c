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
        snprintf(range_start, sizeof(range_start), "%lld",
                 (long long)(seg->start_offset + seg->downloaded));
        snprintf(range_end, sizeof(range_end), "%lld",
                 (long long)seg->end_offset);

        http_client_t http;
        if (http_connect(&http, ctx->url, ctx->timeout_sec) != 0) {
            warn("Worker %d: connect failed - %s", tid, http.last_error);
            segmgr_error(mgr, seg);
            http_close(&http);
            continue;
        }

        char scheme[16], host[256], path[HTTP_MAX_PATH];
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

        int64_t seg_len = seg->end_offset - seg->start_offset + 1;
        int64_t request_start = seg->start_offset + seg->downloaded;
        if (resp.content_range_start != request_start ||
            resp.content_range_end != seg->end_offset ||
            resp.content_range_total <= 0) {
            warn("Worker %d: unexpected Content-Range %lld-%lld/%lld for request %lld-%lld",
                 tid,
                 (long long)resp.content_range_start,
                 (long long)resp.content_range_end,
                 (long long)resp.content_range_total,
                 (long long)request_start,
                 (long long)seg->end_offset);
            segmgr_error(mgr, seg);
            http_close(&http);
            continue;
        }

        seg->socket_fd = (int)http.fd;

        /* 3. Receive data loop */
        uint64_t last_speed_ms = GetTickCount64();
        int64_t chunk_bytes = 0;
        bool last_chunk = false;
        bool transfer_error = false;

        while (!*ctx->interrupted) {
            int64_t remaining = seg_len - seg->downloaded;
            if (remaining <= 0) {
                break;
            }

            int bytes;
            int read_size = remaining < WORK_BUF_SIZE ? (int)remaining : WORK_BUF_SIZE;
            if (resp.is_chunked) {
                bytes = http_read_body_chunked(&http, buf, read_size, &last_chunk);
            } else {
                bytes = http_read_body(&http, buf, read_size);
            }
            if (bytes < 0) {
                /* Error */
                segmgr_error(mgr, seg);
                transfer_error = true;
                break;
            }
            if (bytes == 0) {
                warn("Worker %d: early EOF (%lld/%lld bytes for range %lld-%lld)",
                     tid, (long long)seg->downloaded, (long long)seg_len,
                     (long long)seg->start_offset, (long long)seg->end_offset);
                segmgr_error(mgr, seg);
                transfer_error = true;
                break;
            }

            /* Write at correct offset */
            int64_t write_offset = seg->start_offset + seg->downloaded;
            int written = file_write_at(ctx->output_file, write_offset, buf, bytes);
            if (written != bytes) {
                warn("Worker %d: write error at offset %lld", tid, (long long)write_offset);
                segmgr_error(mgr, seg);
                transfer_error = true;
                break;
            }

            seg->crc32 = crc32_update(seg->crc32, buf, bytes);
            seg->downloaded += bytes;
            InterlockedExchangeAdd64(ctx->global_downloaded, bytes);
            chunk_bytes += bytes;

            if (seg->downloaded >= seg_len) {
                break;
            }

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

        if (!transfer_error && complete) {
            segmgr_complete(mgr, seg);
        }
        /* If error happened, seg state was already set by segmgr_error */
    }

    free(buf);
    free(ctx);
    return 0;
}
