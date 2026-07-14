#include "thread_pool.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORK_BUF_SIZE 65536

HANDLE* thread_pool_start(int count, worker_ctx_t* base_ctx, int* out_count) {
    *out_count = 0;
    HANDLE* threads = (HANDLE*)calloc(count, sizeof(HANDLE));
    if (!threads) return NULL;

    int created = 0;
    for (int i = 0; i < count; i++) {
        worker_ctx_t* ctx = (worker_ctx_t*)malloc(sizeof(worker_ctx_t));
        if (!ctx) continue;
        memcpy(ctx, base_ctx, sizeof(worker_ctx_t));
        ctx->thread_id = created;

        HANDLE thread = CreateThread(NULL, 0, worker_thread_func, ctx, 0, NULL);
        if (!thread) {
            warn("Failed to create worker thread %d", i);
            free(ctx);
        } else {
            threads[created++] = thread;
        }
    }
    *out_count = created;
    return threads;
}

void thread_pool_wait(HANDLE* threads, int count) {
    if (threads && count > 0)
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
    segment_manager_t* mgr = ctx->segmgr;

    /* Allocate receive buffer */
    char* buf = (char*)malloc(WORK_BUF_SIZE);
    if (!buf) {
        warn("Worker %d: out of memory", tid);
        segmgr_abort(mgr);
        free(ctx);
        return 1;
    }

    while (!*ctx->interrupted && !segmgr_has_error(mgr)) {
        /* 1. Acquire a pending segment */
        segment_t* seg = segmgr_acquire_pending(mgr);
        if (!seg && segmgr_all_done(mgr)) break;

        if (!seg) {
            /* Nothing to do - wait for new work or completion */
            if (segmgr_all_done(mgr) || segmgr_has_error(mgr)) break;

            if (segmgr_retry_expired(mgr) > 0)
                continue;

            EnterCriticalSection(&mgr->lock);
            /* No suspended segment is ready yet; retry after the timeout. */
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

        int ticket_refreshes = 0;
retry_request:
        ;
        char request_url[HTTP_MAX_URL];
        unsigned long ticket_generation = 0;
        if (ctx->ticket)
            oss_ticket_snapshot(ctx->ticket, request_url, sizeof(request_url),
                                &ticket_generation);
        else
            snprintf(request_url, sizeof(request_url), "%s", ctx->url);

        http_client_t http;
        if (http_connect(&http, request_url, ctx->timeout_sec, ctx->proxy) != 0) {
            warn("Worker %d: connect failed - %s", tid, http.last_error);
            segmgr_error(mgr, seg);
            http_close(&http);
            continue;
        }

        char scheme[16], host[256], path[HTTP_MAX_PATH];
        int port;
        http_parse_url(request_url, scheme, sizeof(scheme),
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

        if (ctx->ticket && ctx->ticket->enabled &&
            (resp.status_code == 401 || resp.status_code == 403 ||
             resp.status_code == 416) && ticket_refreshes < 4) {
            char refresh_error[512] = {0};
            http_close(&http);
            if (oss_ticket_refresh(ctx->ticket, ticket_generation,
                                   refresh_error, sizeof(refresh_error)) == 0) {
                ticket_refreshes++;
                trace("Worker %d: Plus URL refreshed, retrying range", tid);
                goto retry_request;
            }
            warn("Worker %d: Plus URL refresh failed - %s", tid,
                 refresh_error);
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
            resp.content_range_total != mgr->file_size) {
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

        if (ctx->resource_validator && ctx->resource_validator[0]) {
            char actual[256] = {0};
            if (resp.etag[0])
                snprintf(actual, sizeof(actual), "etag:%s", resp.etag);
            else if (resp.last_modified[0])
                snprintf(actual, sizeof(actual), "last-modified:%s", resp.last_modified);
            if (!actual[0] || strcmp(actual, ctx->resource_validator) != 0) {
                warn("Worker %d: remote file validator changed", tid);
                segmgr_error(mgr, seg);
                http_close(&http);
                continue;
            }
        }

        seg->socket_fd = (int)http.fd;

        /* 3. Receive data loop */
        uint64_t last_speed_ms = GetTickCount64();
        int64_t chunk_bytes = 0;
        bool transfer_error = false;

        while (!*ctx->interrupted && !segmgr_has_error(mgr)) {
            int64_t remaining = seg_len - seg->downloaded;
            if (remaining <= 0) {
                break;
            }

            int bytes;
            int read_size = remaining < WORK_BUF_SIZE ? (int)remaining : WORK_BUF_SIZE;
            bytes = http_read_body(&http, buf, read_size);
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

            uint32_t next_crc = crc32_update(seg->crc32, buf, bytes);
            EnterCriticalSection(&mgr->lock);
            seg->crc32 = next_crc;
            seg->downloaded += bytes;
            LeaveCriticalSection(&mgr->lock);
            InterlockedExchangeAdd64(ctx->global_downloaded, bytes);
            chunk_bytes += bytes;

            if (seg->downloaded >= seg_len) {
                break;
            }

            /* Update speed every 500ms */
            uint64_t now = GetTickCount64();
            if (now - last_speed_ms >= 500) {
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
