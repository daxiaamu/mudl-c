#include "engine.h"
#include "utils.h"
#include "http.h"
#include "file_io.h"
#include "progress.h"
#include "segment.h"
#include "thread_pool.h"
#include "persist.h"
#include "checksum.h"
#include "oss_ticket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>

#define BUF_SIZE 65536

static volatile bool g_interrupted = false;

static void infof(const options_t* opts, const char* fmt, ...);
static void response_validator(const http_response_t* resp, char* out, int out_n);

void engine_interrupt(void) {
    g_interrupted = true;
}

/* Single-thread fallback */
static int download_single(options_t* opts, const char* outpath,
                           char* path, int64_t file_size,
                           oss_ticket_t* ticket);

/* Multi-thread download */
static int download_multi(options_t* opts, const char* outpath,
                          const char* path, int64_t total_size,
                          oss_ticket_t* ticket);


/* Speed tracker */
typedef struct {
    CRITICAL_SECTION lock;
    int64_t          downloaded;
    int64_t          speed;
    uint64_t         last_time;
    int64_t          last_bytes;
} speed_tracker_t;

/* Engine monitor context */
typedef struct {
    segment_manager_t* segmgr;
    options_t*         opts;
    speed_tracker_t*   speed_tracker;
    volatile bool*     running;
} engine_ctx_t;

static void speed_init(speed_tracker_t* st, int64_t initial_downloaded);
static void speed_tick(speed_tracker_t* st, int64_t total_downloaded);

int engine_run(const options_t* options) {
    options_t opts = *options;
    http_global_init();

    oss_ticket_t ticket;
    char ticket_error[512] = {0};
    if (oss_ticket_init(&ticket, opts.url, opts.timeout_sec,
                        opts.user_agent, opts.referer,
                        (const char**)opts.extra_headers, opts.extra_count,
                        &opts.proxy, ticket_error, sizeof(ticket_error)) != 0) {
        fprintf(stderr, "Error: Plus compatibility initialization failed: %s\n",
                ticket_error);
        oss_ticket_destroy(&ticket);
        http_global_cleanup();
        return 1;
    }
    if (ticket.enabled) {
        oss_ticket_snapshot(&ticket, opts.url, sizeof(opts.url), NULL);
        infof(&opts, "MUDL Plus compatibility enabled\n");
    }

    char outpath[MAX_PATH * 2];
    if (file_resolve_output_path(opts.url, opts.dir, opts.output,
                                 outpath, sizeof(outpath)) != 0) {
        fprintf(stderr, "Error: -o/--output expects a filename only. Use -d/--dir for the output directory.\n");
        oss_ticket_destroy(&ticket);
        http_global_cleanup();
        return 1;
    }

    infof(&opts, "URL:     %s\n", opts.url);
    infof(&opts, "Output:  %s\n", outpath);
    infof(&opts, "Threads: %d\n\n", opts.connections);

    char scheme[16], host[256], path[HTTP_MAX_PATH];
    int port;
    http_parse_url(opts.url, scheme, sizeof(scheme),
                   host, sizeof(host), &port, path, sizeof(path));

    /* Probe: GET with Range: bytes=0- to check if server supports Range
       OSS signed URLs reject HEAD (403) but work fine with GET. */
    infof(&opts, "Downloading directly (skipping HEAD probe)\n\n");

    int exit_code;
    http_client_t probe;
    http_response_t probe_resp;
    int64_t file_size = 0;
    bool multi_supported = false;
    int r;
    int redirect_count;
    int probe_ticket_refreshes = 0;
    unsigned long probe_ticket_generation = 0;

probe_connect:
    oss_ticket_snapshot(&ticket, opts.url, sizeof(opts.url),
                        &probe_ticket_generation);
    http_parse_url(opts.url, scheme, sizeof(scheme),
                   host, sizeof(host), &port, path, sizeof(path));

    if (http_connect(&probe, opts.url, opts.timeout_sec, &opts.proxy) != 0) {
        fprintf(stderr, "Error: %s\n", probe.last_error);
        oss_ticket_destroy(&ticket);
        http_global_cleanup();
        return 1;
    }

    r = http_request(&probe, HTTP_GET, path, "0-", NULL,
                     opts.user_agent, opts.referer,
                     (const char**)opts.extra_headers, opts.extra_count,
                     &probe_resp);

    /* Follow redirects (OSS signed URLs often redirect to CDN) */
    redirect_count = 0;
    while (r == 0 && (probe_resp.status_code == 301 ||
           probe_resp.status_code == 302 ||
           probe_resp.status_code == 307 ||
           probe_resp.status_code == 308) &&
           probe_resp.location[0] && redirect_count < 5) {
        redirect_count++;
        infof(&opts, "Redirect %d: %s\n", redirect_count, probe_resp.location);
        http_close(&probe);

        snprintf(opts.url, sizeof(opts.url), "%s", probe_resp.location);
        if (http_connect(&probe, opts.url, opts.timeout_sec, &opts.proxy) != 0) {
            fprintf(stderr, "Redirect error: %s\n", probe.last_error);
            oss_ticket_destroy(&ticket);
            http_global_cleanup();
            return 1;
        }

        char scheme[16], host[256], path_new[HTTP_MAX_PATH];
        int port;
        http_parse_url(opts.url, scheme, sizeof(scheme),
                       host, sizeof(host), &port, path_new, sizeof(path_new));
        snprintf(path, sizeof(path), "%s", path_new);

        r = http_request(&probe, HTTP_GET, path,
                         "0-", NULL,
                         opts.user_agent, opts.referer,
                         (const char**)opts.extra_headers, opts.extra_count,
                         &probe_resp);
    }

    if (r == 0 && ticket.enabled &&
        (probe_resp.status_code == 401 || probe_resp.status_code == 403 ||
         probe_resp.status_code == 416) && probe_ticket_refreshes < 4) {
        http_close(&probe);
        if (oss_ticket_refresh(&ticket, probe_ticket_generation,
                               ticket_error, sizeof(ticket_error)) != 0) {
            fprintf(stderr, "Error: Plus URL refresh failed: %s\n",
                    ticket_error);
            oss_ticket_destroy(&ticket);
            http_global_cleanup();
            return 1;
        }
        probe_ticket_refreshes++;
        goto probe_connect;
    }

    if (r != 0) {
        fprintf(stderr, "Error: %s\n", probe.last_error);
        http_close(&probe);
        oss_ticket_destroy(&ticket);
        http_global_cleanup();
        return 1;
    }

    if (probe_resp.status_code == 206 && probe_resp.content_range_total > 0) {
        file_size = probe_resp.content_range_total;
        multi_supported = true;
        infof(&opts, "Server supports Range (file size: %lld bytes)\n",
               (long long)file_size);
    } else if (probe_resp.status_code == 200 && probe_resp.content_length > 0) {
        file_size = probe_resp.content_length;
        infof(&opts, "File size from Content-Length: %lld bytes\n",
               (long long)file_size);
    } else if (probe_resp.status_code != 200) {
        fprintf(stderr, "Error: HTTP status %d\n", probe_resp.status_code);
        http_close(&probe);
        oss_ticket_destroy(&ticket);
        http_global_cleanup();
        return 1;
    }
    response_validator(&probe_resp, opts.resource_validator,
                       sizeof(opts.resource_validator));
    http_close(&probe);
    oss_ticket_set_current(&ticket, opts.url);

    /* The segmented engine also supports one worker. Keeping Range downloads
       on this path preserves every verified segment when concurrency changes. */
    if (multi_supported) {
        exit_code = download_multi(&opts, outpath, path, file_size, &ticket);
    } else {
        if (file_size > 0)
            infof(&opts, "Using single-thread (connections=%d)\n\n", opts.connections);
        exit_code = download_single(&opts, outpath, path, file_size, &ticket);
    }

    if (exit_code == 0 && opts.checksum[0]) {
        char actual[160];
        char err[256];
        infof(&opts, "Checksum: verifying %s\n", opts.checksum);
        int cr = checksum_verify_file(outpath, opts.checksum,
                                      actual, sizeof(actual),
                                      err, sizeof(err));
        if (cr < 0) {
            fprintf(stderr, "Checksum error: %s\n", err);
            exit_code = 5;
        } else if (cr > 0) {
            fprintf(stderr, "Checksum mismatch: expected %s, actual %s\n",
                    strchr(opts.checksum, '=') ? strchr(opts.checksum, '=') + 1 : opts.checksum,
                    actual);
            exit_code = 5;
        } else {
            infof(&opts, "Checksum OK: %s\n", actual);
        }
    }

    oss_ticket_destroy(&ticket);
    http_global_cleanup();
    return exit_code;
}

/* ===== Single-thread fallback ===== */
static int download_single(options_t* opts, const char* outpath,
                           char* path, int64_t file_size,
                           oss_ticket_t* ticket) {
    /* Check for resume (segments.bin) */
    char segpath[MAX_PATH * 2];
    persist_path(outpath, segpath, sizeof(segpath));
    int64_t resume_pos = 0;
    uint32_t resume_crc32 = 0;
    bool resumed = false;
    if (file_size > 0 && persist_exists(segpath)) {
        segment_manager_t tmp_mgr;
        memset(&tmp_mgr, 0, sizeof(tmp_mgr));
        if (persist_load(segpath, &tmp_mgr, file_size, NULL,
                         opts->resource_validator) == 0 && tmp_mgr.segment_count > 0) {
            resume_pos = tmp_mgr.segments[0].downloaded;
            resume_crc32 = tmp_mgr.segments[0].crc32;
            infof(opts, "Resuming at byte %lld (%.1f%%)\n",
                   (long long)resume_pos,
                   (double)resume_pos / file_size * 100.0);
            segmgr_destroy(&tmp_mgr);
            resumed = true;
        } else {
            if (tmp_mgr.segments || tmp_mgr.pending_queue)
                segmgr_destroy(&tmp_mgr);
        }
    }

    infof(opts, "Method:  single-thread%s\n\n",
           resumed ? " (resume)" : "");
    http_client_t cli;
    unsigned long ticket_generation = 0;
    oss_ticket_snapshot(ticket, opts->url, sizeof(opts->url),
                        &ticket_generation);
    if (http_connect(&cli, opts->url, opts->timeout_sec, &opts->proxy) != 0) {
        fprintf(stderr, "Error: %s\n", cli.last_error);
        return 2;
    }

    progress_t prog;
    progress_init(&prog, opts->progress_mode, file_size, resume_pos, 1);

    file_t f;
    if (file_open(&f, outpath, file_size) != 0) {
        fprintf(stderr, "Error: %s\n", f.last_error);
        http_close(&cli);
        return 1;
    }

    /* Seek to resume position if resuming */
    if (resume_pos > 0) {
        LARGE_INTEGER li;
        li.QuadPart = resume_pos;
        SetFilePointerEx(f.hFile, li, NULL, FILE_BEGIN);
    }

    http_response_t get_resp;
    /* Send Range: bytes=POS- (POS=0 or resume position).
       If server supports Range, it returns 206 + Content-Range with file size.
       If not, it returns 200 with full content. */
    char range_start[32];
    snprintf(range_start, sizeof(range_start), "%lld", (long long)resume_pos);
    int ticket_refreshes = 0;
send_single_request:
    ;
    int r = http_request(&cli, HTTP_GET, path,
                         resume_pos > 0 ? range_start : "0-", NULL,
                         opts->user_agent, opts->referer,
                         (const char**)opts->extra_headers, opts->extra_count,
                         &get_resp);

    /* Follow redirect (OSS signed URLs often redirect to CDN) */
    int redirect_count = 0;
    while (r == 0 && (get_resp.status_code == 301 || get_resp.status_code == 302 ||
           get_resp.status_code == 307 || get_resp.status_code == 308) &&
           get_resp.location[0] && redirect_count < 5) {
        redirect_count++;
        infof(opts, "Redirect %d: %s\n", redirect_count, get_resp.location);
        http_close(&cli);

        /* Update URL and reconnect */
        snprintf(opts->url, sizeof(opts->url), "%s", get_resp.location);
        if (http_connect(&cli, opts->url, opts->timeout_sec, &opts->proxy) != 0) {
            fprintf(stderr, "Redirect error: %s\n", cli.last_error);
            file_close(&f);
            return 2;
        }
        /* Update path */
        char scheme[16], host[256], path_new[HTTP_MAX_PATH];
        int port;
        http_parse_url(opts->url, scheme, sizeof(scheme),
                       host, sizeof(host), &port, path_new, sizeof(path_new));
        snprintf(path, HTTP_MAX_PATH, "%s", path_new);

        r = http_request(&cli, HTTP_GET, path,
                         resume_pos > 0 ? range_start : "0-", NULL,
                         opts->user_agent, opts->referer,
                         (const char**)opts->extra_headers, opts->extra_count,
                         &get_resp);
    }

    if (r == 0 && ticket->enabled &&
        (get_resp.status_code == 401 || get_resp.status_code == 403 ||
         get_resp.status_code == 416) && ticket_refreshes < 4) {
        char refresh_error[512] = {0};
        http_close(&cli);
        if (oss_ticket_refresh(ticket, ticket_generation,
                               refresh_error, sizeof(refresh_error)) != 0) {
            fprintf(stderr, "Error: Plus URL refresh failed: %s\n",
                    refresh_error);
            file_close(&f);
            return 2;
        }
        ticket_refreshes++;
        oss_ticket_snapshot(ticket, opts->url, sizeof(opts->url),
                            &ticket_generation);
        char scheme[16], host[256], path_new[HTTP_MAX_PATH];
        int port;
        http_parse_url(opts->url, scheme, sizeof(scheme), host, sizeof(host),
                       &port, path_new, sizeof(path_new));
        snprintf(path, HTTP_MAX_PATH, "%s", path_new);
        if (http_connect(&cli, opts->url, opts->timeout_sec, &opts->proxy) != 0) {
            fprintf(stderr, "Error: %s\n", cli.last_error);
            file_close(&f);
            return 2;
        }
        redirect_count = 0;
        goto send_single_request;
    }

    oss_ticket_set_current(ticket, opts->url);

    if (r != 0) {
        fprintf(stderr, "Error: %s\n", cli.last_error);
        file_close(&f);
        http_close(&cli);
        return 2;
    }

    if (get_resp.status_code != 200 && get_resp.status_code != 206) {
        fprintf(stderr, "Error: HTTP status %d\n", get_resp.status_code);
        file_close(&f);
        http_close(&cli);
        return 2;
    }
    if (resume_pos > 0 &&
        (get_resp.status_code != 206 ||
         get_resp.content_range_start != resume_pos ||
         get_resp.content_range_total != file_size)) {
        fprintf(stderr, "Error: server did not honor the resume range\n");
        file_close(&f);
        http_close(&cli);
        return 2;
    }
    if (get_resp.status_code == 206 && file_size > 0 &&
        (get_resp.content_range_start != resume_pos ||
         get_resp.content_range_total != file_size)) {
        fprintf(stderr, "Error: unexpected Content-Range in single-thread response\n");
        file_close(&f);
        http_close(&cli);
        return 2;
    }
    if (opts->resource_validator[0]) {
        char actual_validator[256];
        response_validator(&get_resp, actual_validator, sizeof(actual_validator));
        if (!actual_validator[0] ||
            strcmp(actual_validator, opts->resource_validator) != 0) {
            fprintf(stderr, "Error: remote file validator changed during download\n");
            file_close(&f);
            http_close(&cli);
            return 2;
        }
    }

    char* buf = (char*)malloc(BUF_SIZE);
    if (!buf) { file_close(&f); http_close(&cli); return 1; }

    int64_t downloaded = resume_pos;
    uint32_t download_crc32 = resume_crc32;
    bool download_error = false;
    speed_tracker_t st;
    speed_init(&st, resume_pos);
    uint64_t last_save_ms = GetTickCount64();

    while (!g_interrupted) {
        int bytes = http_read_body(&cli, buf, BUF_SIZE);
        if (bytes < 0) {
            fprintf(stderr, "\nError: %s\n", cli.last_error);
            download_error = true;
            break;
        }
        if (bytes == 0) break;

        int written = file_write(&f, buf, bytes);
        if (written != bytes) {
            fprintf(stderr, "\nError: %s\n", f.last_error);
            download_error = true;
            break;
        }
        download_crc32 = crc32_update(download_crc32, buf, bytes);
        downloaded += bytes;
        speed_tick(&st, downloaded);
        progress_update(&prog, downloaded, st.speed, 1, 1);

        /* Periodic save for resume (every 10s) */
        if (file_size > 0) {
            uint64_t now = GetTickCount64();
            if (now - last_save_ms >= 3000) {
                segment_manager_t save_mgr;
                memset(&save_mgr, 0, sizeof(save_mgr));
                save_mgr.segment_count = 1;
                save_mgr.complete_count = 0;
                save_mgr.file_size = file_size;
                save_mgr.segments = (segment_t*)calloc(1, sizeof(segment_t));
                if (save_mgr.segments) {
                    save_mgr.segments[0].index = 0;
                    save_mgr.segments[0].start_offset = 0;
                    save_mgr.segments[0].end_offset = file_size - 1;
                    save_mgr.segments[0].downloaded = downloaded;
                    save_mgr.segments[0].state = SEG_DOWNLOADING;
                    save_mgr.segments[0].crc32 = download_crc32;
                    InitializeCriticalSection(&save_mgr.lock);
                    persist_save(segpath, &save_mgr, file_size, 1,
                                 opts->resource_validator);
                    DeleteCriticalSection(&save_mgr.lock);
                    free(save_mgr.segments);
                }
                last_save_ms = now;
            }
        }
    }

    free(buf);
    progress_update(&prog, downloaded, st.speed, 1, 1);
    progress_done(&prog);
    file_close(&f);
    http_close(&cli);

    if (g_interrupted) {
        /* Save state for resume on Ctrl+C */
        if (file_size > 0 && downloaded > 0) {
            segment_manager_t save_mgr;
            memset(&save_mgr, 0, sizeof(save_mgr));
            save_mgr.segment_count = 1;
            save_mgr.complete_count = 0;
            save_mgr.file_size = file_size;
            save_mgr.segments = (segment_t*)calloc(1, sizeof(segment_t));
            if (save_mgr.segments) {
                save_mgr.segments[0].index = 0;
                save_mgr.segments[0].start_offset = 0;
                save_mgr.segments[0].end_offset = file_size - 1;
                save_mgr.segments[0].downloaded = downloaded;
                save_mgr.segments[0].state = SEG_DOWNLOADING;
                save_mgr.segments[0].crc32 = download_crc32;
                InitializeCriticalSection(&save_mgr.lock);
                persist_save(segpath, &save_mgr, file_size, 1,
                             opts->resource_validator);
                DeleteCriticalSection(&save_mgr.lock);
                free(save_mgr.segments);
            }
        }
        infof(opts, "\nPaused. Run again with new URL to resume.\n");
        return 4;
    }

    if (download_error) {
        return 2;
    }

    if (file_size > 0 && downloaded != file_size) {
        fprintf(stderr,
                "\nError: incomplete download (%lld/%lld bytes). Resume file kept.\n",
                (long long)downloaded, (long long)file_size);
        segment_manager_t save_mgr;
        memset(&save_mgr, 0, sizeof(save_mgr));
        save_mgr.segment_count = 1;
        save_mgr.complete_count = 0;
        save_mgr.file_size = file_size;
        save_mgr.segments = (segment_t*)calloc(1, sizeof(segment_t));
        if (save_mgr.segments) {
            save_mgr.segments[0].index = 0;
            save_mgr.segments[0].start_offset = 0;
            save_mgr.segments[0].end_offset = file_size - 1;
            save_mgr.segments[0].downloaded = downloaded;
            save_mgr.segments[0].state = SEG_DOWNLOADING;
            save_mgr.segments[0].crc32 = download_crc32;
            InitializeCriticalSection(&save_mgr.lock);
            persist_save(segpath, &save_mgr, file_size, 1,
                         opts->resource_validator);
            DeleteCriticalSection(&save_mgr.lock);
            free(save_mgr.segments);
        }
        return 3;
    }

    /* Remove resume file on clean completion */
    if (file_size > 0) persist_remove(segpath);
    if (opts->progress_mode == PROGRESS_BAR && !opts->quiet) {
        printf("DONE: %s (%lld bytes)\n", outpath, (long long)downloaded);
    }
    return 0;
}

/* ===== Multi-thread download ===== */
static int download_multi(options_t* opts, const char* outpath,
                          const char* path, int64_t total_size,
                          oss_ticket_t* ticket) {
    int conn = opts->connections;
    if (conn == 1)
        infof(opts, "Method:  segmented (1 connection)\n\n");
    else
        infof(opts, "Method:  multi-thread (%d connections)\n\n", conn);

    /* Build segments.bin path */
    char segpath[MAX_PATH * 2];
    persist_path(outpath, segpath, sizeof(segpath));

    /* Try resume from segments.bin */
    segment_manager_t segmgr;
    bool resumed = false;
    if (persist_exists(segpath)) {
        trace("Resume file found: %s", segpath);
        int64_t existing_size = file_size(outpath);
        if (existing_size < 0) {
            infof(opts, "Resume state ignored: output file is missing\n");
        } else if (existing_size < total_size) {
            infof(opts, "Resume state ignored: output file is smaller than expected\n");
        } else {
            memset(&segmgr, 0, sizeof(segmgr));
            if (persist_load(segpath, &segmgr, total_size, NULL,
                             opts->resource_validator) == 0) {
                infof(opts, "Resuming from segments.bin (%d segments, %lld pending)\n",
                       segmgr.segment_count,
                       (long long)(segmgr.segment_count - segmgr.complete_count));
                resumed = true;
            } else {
                trace("Resume file invalid, starting fresh");
            }
        }
    }

    if (!resumed) {
        /* Fresh start */
        if (segmgr_init(&segmgr, total_size, conn) != 0) {
            fprintf(stderr, "Error: failed to init segment manager\n");
            return 1;
        }
        infof(opts, "Segments: %d\n", segmgr.segment_count);
    } else {
        int old_count = segmgr.segment_count;
        int pending = segmgr_expand_pending(&segmgr, conn);
        if (pending < 0) {
            fprintf(stderr, "Error: failed to expand resume segments\n");
            segmgr_destroy(&segmgr);
            return 1;
        }
        if (segmgr.segment_count > old_count) {
            infof(opts, "Resume segments expanded: %d -> %d for %d connections\n",
                  old_count, segmgr.segment_count, conn);
        }
    }

    segmgr.max_retries = opts->max_retries;

    /* Open output file (must exist for resume) */
    file_t output_file;
    if (file_open(&output_file, outpath, total_size) != 0) {
        fprintf(stderr, "Error: %s\n", output_file.last_error);
        segmgr_destroy(&segmgr);
        return 1;
    }

    /* Speed tracker */
    int64_t initial_downloaded = segmgr_total_downloaded(&segmgr);
    speed_tracker_t st;
    speed_init(&st, initial_downloaded);

    /* Worker context */
    worker_ctx_t base_ctx;
    memset(&base_ctx, 0, sizeof(base_ctx));
    base_ctx.url = opts->url;
    base_ctx.user_agent = opts->user_agent;
    base_ctx.referer = opts->referer;
    base_ctx.extra_headers = (const char**)opts->extra_headers;
    base_ctx.extra_count = opts->extra_count;
    base_ctx.timeout_sec = opts->timeout_sec;
    base_ctx.resource_validator = opts->resource_validator;
    base_ctx.proxy = &opts->proxy;
    base_ctx.segmgr = &segmgr;
    base_ctx.output_file = &output_file;
    base_ctx.interrupted = &g_interrupted;
    base_ctx.global_downloaded = &st.downloaded;
    base_ctx.ticket = ticket;

    progress_t prog;
    progress_init(&prog, opts->progress_mode, total_size,
                  initial_downloaded, conn);

    /* Start workers */
    int actual_count;
    HANDLE* workers = thread_pool_start(conn, &base_ctx, &actual_count);

    if (!workers || actual_count == 0) {
        fprintf(stderr, "Error: failed to create worker threads\n");
        thread_pool_cleanup(workers, actual_count);
        file_close(&output_file);
        segmgr_destroy(&segmgr);
        return 1;
    }

    infof(opts, "Started %d workers\n", actual_count);

    uint64_t last_save_ms = GetTickCount64();

    while (!segmgr_all_done(&segmgr) && !g_interrupted && !segmgr_has_error(&segmgr)) {
        Sleep(500);

        /* Periodic save every 5 seconds */
        uint64_t now = GetTickCount64();
        if (now - last_save_ms >= 5000) {
            persist_save(segpath, &segmgr, total_size, conn,
                         opts->resource_validator);
            last_save_ms = now;
        }

        int64_t total = segmgr_total_downloaded(&segmgr);
        speed_tick(&st, total);

        int active = 0;
        EnterCriticalSection(&segmgr.lock);
        active = segmgr.active_count;
        LeaveCriticalSection(&segmgr.lock);

        progress_update(&prog, total, st.speed, active, actual_count);
    }

    /* Final progress update as soon as all bytes are done, before cleanup waits. */
    int64_t final_total = segmgr_total_downloaded(&segmgr);
    bool segment_failed = segmgr_has_error(&segmgr);
    if (!g_interrupted && segment_failed) {
        fprintf(stderr,
                "\nError: one or more segments failed. Resume file kept.\n");
        persist_save(segpath, &segmgr, total_size, conn,
                     opts->resource_validator);
    } else if (!g_interrupted && total_size > 0 && final_total != total_size) {
        fprintf(stderr,
                "\nError: incomplete download (%lld/%lld bytes). Resume file kept.\n",
                (long long)final_total, (long long)total_size);
        persist_save(segpath, &segmgr, total_size, conn,
                     opts->resource_validator);
    } else if (!g_interrupted) {
        progress_update(&prog, final_total, 0, 0, actual_count);
        progress_done(&prog);
        if (opts->progress_mode == PROGRESS_BAR && !opts->quiet) {
            printf("DONE: %s (%lld bytes)\n", outpath, (long long)final_total);
        }
    }

    /* Wake idle workers after user-visible completion. */
    WakeAllConditionVariable(&segmgr.cv_new_work);

    /* Wait for workers to finish */
    thread_pool_wait(workers, actual_count);
    thread_pool_cleanup(workers, actual_count);

    if (segment_failed) {
        final_total = segmgr_total_downloaded(&segmgr);
        persist_save(segpath, &segmgr, total_size, conn,
                     opts->resource_validator);
    }

    if (g_interrupted) {
        /* Save state for resume on Ctrl+C */
        persist_save(segpath, &segmgr, total_size, conn,
                     opts->resource_validator);
        file_close(&output_file);
        infof(opts, "\nPaused. Run again with same URL/args to resume.\n");
        segmgr_destroy(&segmgr);
        return 4;
    }

    file_close(&output_file);

    if (segment_failed) {
        segmgr_destroy(&segmgr);
        return 2;
    }

    if (total_size > 0 && final_total != total_size) {
        segmgr_destroy(&segmgr);
        return 3;
    }

    /* Remove resume file on clean completion */
    persist_remove(segpath);
    segmgr_destroy(&segmgr);
    return 0;
}

/* ===== Speed tracker ===== */
void speed_init(speed_tracker_t* st, int64_t initial_downloaded) {
    InitializeCriticalSection(&st->lock);
    st->downloaded = initial_downloaded;
    st->speed = 0;
    st->last_time = GetTickCount64();
    st->last_bytes = initial_downloaded;
}

void speed_tick(speed_tracker_t* st, int64_t total_downloaded) {
    uint64_t now = GetTickCount64();
    EnterCriticalSection(&st->lock);
    uint64_t dt = now - st->last_time;
    if (dt >= 500) {
        int64_t diff = total_downloaded - st->last_bytes;
        st->speed = diff * 1000 / (int64_t)dt;
        st->last_time = now;
        st->last_bytes = total_downloaded;
    }
    LeaveCriticalSection(&st->lock);
}




static void infof(const options_t* opts, const char* fmt, ...) {
    if (opts && opts->quiet)
        return;

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void response_validator(const http_response_t* resp, char* out, int out_n) {
    out[0] = 0;
    if (resp->etag[0])
        snprintf(out, out_n, "etag:%s", resp->etag);
    else if (resp->last_modified[0])
        snprintf(out, out_n, "last-modified:%s", resp->last_modified);
}
/* ===== Engine monitor thread ===== */
/* Periodically checks speeds and decides to upgrade/degrade threads.
   Also handles SUSPENDED segment retry timeouts. */
DWORD WINAPI engine_monitor_thread(LPVOID param) {
    engine_ctx_t* ctx = (engine_ctx_t*)param;

    while (*ctx->running && !g_interrupted) {
        for (int i = 0; i < 20 && *ctx->running && !g_interrupted; i++) {
            Sleep(100);
        }
        if (!*ctx->running || g_interrupted) break;

        segment_manager_t* mgr = ctx->segmgr;
        int max_conn = ctx->opts->connections;

        /* 1. Handle SUSPENDED segments timeout */
        segmgr_retry_expired(mgr);

        /* 2. Dynamic thread count adjustment */
        if (mgr->active_count < 2) continue;  /* need at least 2 to compare */

        int64_t baseline = segmgr_get_baseline_speed(mgr);
        if (baseline <= 0) continue;  /* not enough data */

        /* Total speed = baseline * active_count (approx, since baseline is avg of top ~3) */
        /* Get speeds for calculation */
        int64_t speeds[8];
        int active = segmgr_get_active_speeds(mgr, speeds, 8);
        if (active < 2) continue;

        int64_t total_speed = 0;
        for (int i = 0; i < active; i++) total_speed += speeds[i];

        /* Degrade: if total speed < 1.3x baseline, threads are fighting */
        if (total_speed < baseline * 13 / 10 && active > 2) {
            /* Suspend the slowest segment */
            if (segmgr_suspend_slowest(mgr) == 0) {
                trace("Engine: DEGRADE (total=%lld, baseline=%lld, active=%d->%d)",
                      (long long)total_speed, (long long)baseline, active, active - 1);
            }
        }
        /* Upgrade: if total speed > 0.8 * baseline * active, we can add more */
        else if (total_speed > baseline * active * 8 / 10 && active < max_conn) {
            /* Check if there's still un-assigned work */
            int64_t remaining = segmgr_total_remaining(mgr);
            if (remaining > 1024 * 1024) {  /* at least 1MB left */
                /* The work will be picked up by workers via cv_new_work signal.
                   We just make sure the pending queue has entries.
                   If a worker is idle, it will pick it up. */
                trace("Engine: could UPGRADE (total=%lld, baseline=%lld, active=%d/%d, rem=%lld)",
                      (long long)total_speed, (long long)baseline,
                      active, max_conn, (long long)remaining);
            }
        }
    }
    return 0;
}
