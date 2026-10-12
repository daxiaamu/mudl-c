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

static void source_failure(download_source_t* source, bool permanent,
                           bool can_disable_transient, int tid,
                           int source_index, const char* reason) {
    LONG failures = InterlockedIncrement(&source->failures);
    if (permanent || (can_disable_transient && failures >= 3)) {
        if (InterlockedExchange(&source->disabled, 1) == 0)
            warn("Worker %d: source %d disabled - %s", tid, source_index + 1, reason);
    } else {
        trace("Worker %d: source %d failed (%ld%s) - %s",
              tid, source_index + 1, failures,
              can_disable_transient ? "/3" : "", reason);
    }
}

/* 0: complete, 1: try another source, 2: local fatal/interrupted,
   3: segment ownership was returned to the scheduler. */
static int download_from_source(worker_ctx_t* ctx, segment_t* seg,
                                download_source_t* source, int source_index,
                                char* buf) {
    int tid = ctx->thread_id;
    segment_manager_t* mgr = ctx->segmgr;
    int64_t seg_len = seg->end_offset - seg->start_offset + 1;
    int64_t request_start = seg->start_offset + seg->downloaded;
    char range_start[32], range_end[32];
    snprintf(range_start, sizeof(range_start), "%lld", (long long)request_start);
    snprintf(range_end, sizeof(range_end), "%lld", (long long)seg->end_offset);

    http_client_t http;
    if (http_connect(&http, source->url, ctx->timeout_sec, ctx->proxy,
                     ctx->interface_name) != 0) {
        source_failure(source, false, ctx->source_count > 1,
                       tid, source_index, http.last_error);
        http_close(&http);
        return 1;
    }

    http_response_t resp;
    int r = http_request(&http, HTTP_GET, source->path,
                         range_start, range_end,
                         ctx->user_agent, ctx->referer,
                         ctx->extra_headers, ctx->extra_count, &resp);
    trace("Worker %d: source=%d Range: bytes=%s-%s -> status=%d",
          tid, source_index + 1, range_start, range_end, resp.status_code);
    if (r != 0) {
        source_failure(source, false, ctx->source_count > 1,
                       tid, source_index, http.last_error);
        http_close(&http);
        return 1;
    }

    if (resp.status_code != 206) {
        bool permanent = resp.status_code == 401 || resp.status_code == 403 ||
                         resp.status_code == 404 || resp.status_code == 410 ||
                         resp.status_code == 416;
        char reason[64];
        snprintf(reason, sizeof(reason), "HTTP status %d", resp.status_code);
        source_failure(source, permanent, ctx->source_count > 1,
                       tid, source_index, reason);
        http_close(&http);
        return 1;
    }

    if (resp.content_range_start != request_start ||
        resp.content_range_end != seg->end_offset ||
        resp.content_range_total != mgr->file_size) {
        source_failure(source, true, ctx->source_count > 1, tid, source_index,
                       "unexpected Content-Range");
        http_close(&http);
        return 1;
    }

    if (source->validator[0]) {
        char actual[320] = {0};
        if (resp.etag[0])
            snprintf(actual, sizeof(actual), "etag:%s", resp.etag);
        else if (resp.last_modified[0])
            snprintf(actual, sizeof(actual), "last-modified:%s", resp.last_modified);
        if (!actual[0] || strcmp(actual, source->validator) != 0) {
            source_failure(source, true, ctx->source_count > 1,
                           tid, source_index,
                           "remote validator changed");
            http_close(&http);
            return 1;
        }
    }

    InterlockedExchange(&source->failures, 0);
    seg->socket_fd = (int)http.fd;
    uint64_t last_speed_ms = GetTickCount64();
    int64_t chunk_bytes = 0;

    while (!*ctx->interrupted && !segmgr_has_error(mgr)) {
        EnterCriticalSection(&mgr->lock);
        bool owned = seg->state == SEG_DOWNLOADING;
        int64_t downloaded = seg->downloaded;
        LeaveCriticalSection(&mgr->lock);
        if (!owned) {
            http_close(&http);
            return 3;
        }

        int64_t remaining = seg_len - downloaded;
        if (remaining <= 0) break;
        int read_size = remaining < WORK_BUF_SIZE ? (int)remaining : WORK_BUF_SIZE;
        int bytes = http_read_body(&http, buf, read_size);
        if (bytes <= 0) {
            source_failure(source, false, ctx->source_count > 1,
                           tid, source_index,
                           bytes < 0 ? http.last_error : "early EOF");
            http_close(&http);
            return 1;
        }

        int64_t write_offset = seg->start_offset + downloaded;
        int written = file_write_at(ctx->output_file, write_offset, buf, bytes);
        if (written != bytes) {
            warn("Worker %d: write error at offset %lld", tid,
                 (long long)write_offset);
            http_close(&http);
            segmgr_error(mgr, seg);
            segmgr_abort(mgr);
            return 2;
        }

        uint32_t next_crc = crc32_update(seg->crc32, buf, bytes);
        EnterCriticalSection(&mgr->lock);
        seg->crc32 = next_crc;
        seg->downloaded += bytes;
        LeaveCriticalSection(&mgr->lock);
        InterlockedExchangeAdd64(ctx->global_downloaded, bytes);
        chunk_bytes += bytes;

        uint64_t now = GetTickCount64();
        if (now - last_speed_ms >= 500) {
            seg->speed_bps = chunk_bytes * 1000 / (int64_t)(now - last_speed_ms + 1);
            chunk_bytes = 0;
            last_speed_ms = now;
            seg->last_activity_ms = now;
        }
    }

    http_close(&http);
    if (*ctx->interrupted || segmgr_has_error(mgr)) return 2;
    return 0;
}

DWORD WINAPI worker_thread_func(LPVOID param) {
    worker_ctx_t* ctx = (worker_ctx_t*)param;
    segment_manager_t* mgr = ctx->segmgr;
    char* buf = (char*)malloc(WORK_BUF_SIZE);
    if (!buf) {
        warn("Worker %d: out of memory", ctx->thread_id);
        segmgr_abort(mgr);
        free(ctx);
        return 1;
    }

    while (!*ctx->interrupted && !segmgr_has_error(mgr)) {
        segment_t* seg = segmgr_acquire_pending(mgr);
        if (!seg && segmgr_all_done(mgr)) break;
        if (!seg) {
            if (segmgr_all_done(mgr) || segmgr_has_error(mgr)) break;
            if (segmgr_retry_expired(mgr) > 0) continue;
            EnterCriticalSection(&mgr->lock);
            SleepConditionVariableCS(&mgr->cv_new_work, &mgr->lock, 1000);
            LeaveCriticalSection(&mgr->lock);
            continue;
        }

        LONG start = InterlockedIncrement(ctx->next_source);
        bool complete = false;
        bool ownership_lost = false;
        for (int attempt = 0; attempt < ctx->source_count; attempt++) {
            int index = (int)((start + attempt) % ctx->source_count);
            download_source_t* source = &ctx->sources[index];
            if (InterlockedCompareExchange(&source->disabled, 0, 0)) continue;
            int result = download_from_source(ctx, seg, source, index, buf);
            if (result == 0) {
                complete = true;
                break;
            }
            if (result == 2) break;
            if (result == 3) {
                ownership_lost = true;
                break;
            }
        }

        if (complete)
            segmgr_complete(mgr, seg);
        else if (!ownership_lost && !*ctx->interrupted && !segmgr_has_error(mgr))
            segmgr_error(mgr, seg);
    }

    free(buf);
    free(ctx);
    return 0;
}
