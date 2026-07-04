#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "utils.h"
#include "http.h"
#include "file_io.h"
#include "progress.h"
#include "segment.h"
#include "thread_pool.h"
#include "persist.h"

#define VERSION "0.5.2"
#define DEFAULT_CONNECTIONS 8
#define DEFAULT_TIMEOUT 30
#define DEFAULT_RETRY 5
#define BUF_SIZE 65536

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

typedef struct {
    char        url[HTTP_MAX_URL];
    char        output[256];
    char        dir[MAX_PATH];
    int         connections;
    int         timeout_sec;
    int         max_retries;
    bool        quiet;
    progress_mode_t progress_mode;
    char        user_agent[256];
    char        referer[1024];
    char*       extra_headers[32];
    int         extra_count;
    bool        help;
    bool        version;
} options_t;

static volatile bool g_interrupted = false;

static void parse_args(options_t* opts, int argc, char** argv);
static void print_help(void);
static void print_version(void);
static void sig_handler(int sig);
DWORD WINAPI engine_monitor_thread(LPVOID param);

/* Fix Chinese output on Windows console */
static void init_console(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    /* Enable VT escape sequences */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(h, mode);
    }
}

/* Single-thread fallback */
static int download_single(options_t* opts, const char* outpath,
                           char* path, int64_t file_size);

/* Multi-thread download */
static int download_multi(options_t* opts, const char* outpath,
                          const char* path, int64_t total_size);


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

static void speed_init(speed_tracker_t* st);
static void speed_tick(speed_tracker_t* st, int64_t total_downloaded);

int main(int argc, char** argv) {
    init_console();
    options_t opts;
    parse_args(&opts, argc, argv);
    if (opts.help) { print_help(); return 0; }
    if (opts.version) { print_version(); return 0; }
    if (opts.url[0] == 0) {
        fprintf(stderr, "Error: URL required. Use -h for help.\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    http_global_init();

    /* Determine output file path */
    char filename[256];
    if (opts.output[0]) {
        strncpy(filename, opts.output, sizeof(filename) - 1);
    } else {
        file_name_from_url(opts.url, filename, sizeof(filename), NULL);
        if (filename[0] == 0)
            strncpy(filename, "download", sizeof(filename) - 1);
    }

    char outpath[MAX_PATH * 2];
    file_make_safe_path(opts.dir[0] ? opts.dir : ".", filename,
                        outpath, sizeof(outpath));

    printf("URL:     %s\n", opts.url);
    printf("Output:  %s\n", outpath);
    printf("Threads: %d\n\n", opts.connections);

    char scheme[16], host[256], path[HTTP_MAX_PATH];
    int port;
    http_parse_url(opts.url, scheme, sizeof(scheme),
                   host, sizeof(host), &port, path, sizeof(path));

    /* Probe: GET with Range: bytes=0- to check if server supports Range
       OSS signed URLs reject HEAD (403) but work fine with GET. */
    printf("Downloading directly (skipping HEAD probe)\n\n");

    int exit_code;
    http_client_t probe;
    http_response_t probe_resp;
    int64_t file_size = 0;
    bool multi_supported = false;

    if (http_connect(&probe, opts.url) == 0) {
        int r = http_request(&probe, HTTP_GET, path, "0-", NULL,
                             opts.user_agent, opts.referer,
                             (const char**)opts.extra_headers, opts.extra_count,
                             &probe_resp);

        /* Follow redirects (OSS signed URLs often redirect to CDN) */
        int redirect_count = 0;
        while (r == 0 && (probe_resp.status_code == 301 ||
               probe_resp.status_code == 302 ||
               probe_resp.status_code == 307 ||
               probe_resp.status_code == 308) &&
               probe_resp.location[0] && redirect_count < 5) {
            redirect_count++;
            printf("Redirect %d: %s\n", redirect_count, probe_resp.location);
            http_close(&probe);

            snprintf(opts.url, sizeof(opts.url), "%s", probe_resp.location);
            if (http_connect(&probe, opts.url) != 0) break;

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

        if (r == 0) {
            if (probe_resp.status_code == 206) {
                file_size = probe_resp.content_range_total;
                multi_supported = true;
                printf("Server supports Range (file size: %lld bytes)\n",
                       (long long)file_size);
            } else if (probe_resp.status_code == 200 && probe_resp.content_length > 0) {
                file_size = probe_resp.content_length;
                printf("File size from Content-Length: %lld bytes\n",
                       (long long)file_size);
            }
        }
        http_close(&probe);
    }

    if (multi_supported && opts.connections > 1) {
        exit_code = download_multi(&opts, outpath, path, file_size);
    } else {
        if (file_size > 0)
            printf("Using single-thread (connections=%d)\n\n", opts.connections);
        exit_code = download_single(&opts, outpath, path, file_size);
    }

    http_global_cleanup();
    return exit_code;
}

/* ===== Single-thread fallback ===== */
static int download_single(options_t* opts, const char* outpath,
                           char* path, int64_t file_size) {
    /* Check for resume (segments.bin) */
    char segpath[MAX_PATH * 2];
    persist_path(outpath, segpath, sizeof(segpath));
    int64_t resume_pos = 0;
    uint32_t resume_crc32 = 0;
    bool resumed = false;
    if (file_size > 0 && persist_exists(segpath)) {
        segment_manager_t tmp_mgr;
        memset(&tmp_mgr, 0, sizeof(tmp_mgr));
        if (persist_load(segpath, &tmp_mgr, file_size, NULL) == 0 && tmp_mgr.segment_count > 0) {
            resume_pos = tmp_mgr.segments[0].downloaded;
            resume_crc32 = tmp_mgr.segments[0].crc32;
            printf("Resuming at byte %lld (%.1f%%)\n",
                   (long long)resume_pos,
                   (double)resume_pos / file_size * 100.0);
            segmgr_destroy(&tmp_mgr);
            resumed = true;
        } else {
            if (tmp_mgr.segments || tmp_mgr.pending_queue)
                segmgr_destroy(&tmp_mgr);
        }
    }

    printf("Method:  single-thread%s\n\n",
           resumed ? " (resume)" : "");
    http_client_t cli;
    if (http_connect(&cli, opts->url) != 0) {
        fprintf(stderr, "Error: %s\n", cli.last_error);
        return 2;
    }

    progress_t prog;
    progress_init(&prog, opts->progress_mode, file_size, 1);

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
        printf("Redirect %d: %s\n", redirect_count, get_resp.location);
        http_close(&cli);

        /* Update URL and reconnect */
        snprintf(opts->url, sizeof(opts->url), "%s", get_resp.location);
        if (http_connect(&cli, opts->url) != 0) {
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

    if (r != 0) {
        fprintf(stderr, "Error: %s\n", cli.last_error);
        file_close(&f);
        http_close(&cli);
        return 2;
    }

    char* buf = (char*)malloc(BUF_SIZE);
    if (!buf) { file_close(&f); http_close(&cli); return 1; }

    int64_t downloaded = resume_pos;
    uint32_t download_crc32 = resume_crc32;
    bool download_error = false;
    speed_tracker_t st;
    speed_init(&st);
    uint64_t last_save_ms = GetTickCount64();

    while (!g_interrupted) {
        int bytes = http_read_body(&cli, buf, BUF_SIZE);
        if (bytes < 0) {
            fprintf(stderr, "\nError: %s\n", cli.last_error);
            download_error = true;
            break;
        }
        if (bytes == 0) break;

        file_write(&f, buf, bytes);
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
                    persist_save(segpath, &save_mgr, file_size, 1);
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
                persist_save(segpath, &save_mgr, file_size, 1);
                DeleteCriticalSection(&save_mgr.lock);
                free(save_mgr.segments);
            }
        }
        printf("\nPaused. Run again with new URL to resume.\n");
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
            persist_save(segpath, &save_mgr, file_size, 1);
            DeleteCriticalSection(&save_mgr.lock);
            free(save_mgr.segments);
        }
        return 3;
    }

    /* Remove resume file on clean completion */
    if (file_size > 0) persist_remove(segpath);
    if (opts->progress_mode == PROGRESS_BAR) {
        printf("DONE: %s (%lld bytes)\n", outpath, (long long)downloaded);
    }
    return 0;
}

/* ===== Multi-thread download ===== */
static int download_multi(options_t* opts, const char* outpath,
                          const char* path, int64_t total_size) {
    int conn = opts->connections;
    printf("Method:  multi-thread (%d connections)\n\n", conn);

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
            printf("Resume state ignored: output file is missing\n");
        } else if (existing_size < total_size) {
            printf("Resume state ignored: output file is smaller than expected\n");
        } else {
            memset(&segmgr, 0, sizeof(segmgr));
            if (persist_load(segpath, &segmgr, total_size, NULL) == 0) {
                printf("Resuming from segments.bin (%d segments, %lld pending)\n",
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
        printf("Segments: %d\n", segmgr.segment_count);
    }

    /* Open output file (must exist for resume) */
    file_t output_file;
    if (file_open(&output_file, outpath, total_size) != 0) {
        fprintf(stderr, "Error: %s\n", output_file.last_error);
        segmgr_destroy(&segmgr);
        return 1;
    }

    /* Speed tracker */
    speed_tracker_t st;
    speed_init(&st);

    /* Worker context */
    worker_ctx_t base_ctx;
    memset(&base_ctx, 0, sizeof(base_ctx));
    base_ctx.url = opts->url;
    base_ctx.user_agent = opts->user_agent;
    base_ctx.referer = opts->referer;
    base_ctx.extra_headers = (const char**)opts->extra_headers;
    base_ctx.extra_count = opts->extra_count;
    base_ctx.segmgr = &segmgr;
    base_ctx.output_file = &output_file;
    base_ctx.interrupted = &g_interrupted;
    base_ctx.global_downloaded = &st.downloaded;
    base_ctx.global_speed = &st.speed;
    base_ctx.speed_lock = &st.lock;

    /* Start workers */
    int actual_count;
    HANDLE* workers = thread_pool_start(conn, &base_ctx, &actual_count);

    if (!workers || actual_count == 0) {
        fprintf(stderr, "Error: failed to create worker threads\n");
        file_close(&output_file);
        segmgr_destroy(&segmgr);
        return 1;
    }

    printf("Started %d workers\n", actual_count);

    /* Progress / monitor loop */
    progress_t prog;
    progress_init(&prog, opts->progress_mode, total_size, actual_count);

    uint64_t last_save_ms = GetTickCount64();

    while (!segmgr_all_done(&segmgr) && !g_interrupted && !segmgr_has_error(&segmgr)) {
        Sleep(500);

        /* Periodic save every 5 seconds */
        uint64_t now = GetTickCount64();
        if (now - last_save_ms >= 5000) {
            persist_save(segpath, &segmgr, total_size, conn);
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
        persist_save(segpath, &segmgr, total_size, conn);
    } else if (!g_interrupted && total_size > 0 && final_total != total_size) {
        fprintf(stderr,
                "\nError: incomplete download (%lld/%lld bytes). Resume file kept.\n",
                (long long)final_total, (long long)total_size);
        persist_save(segpath, &segmgr, total_size, conn);
    } else if (!g_interrupted) {
        progress_update(&prog, final_total, 0, 0, actual_count);
        progress_done(&prog);
        if (opts->progress_mode == PROGRESS_BAR) {
            printf("DONE: %s (%lld bytes)\n", outpath, (long long)final_total);
        }
    }

    /* Wake idle workers after user-visible completion. */
    WakeAllConditionVariable(&segmgr.cv_new_work);

    /* Wait for workers to finish */
    thread_pool_wait(workers, actual_count);
    thread_pool_cleanup(workers, actual_count);

    if (g_interrupted) {
        /* Save state for resume on Ctrl+C */
        persist_save(segpath, &segmgr, total_size, conn);
        file_close(&output_file);
        printf("\nPaused. Run again with same URL/args to resume.\n");
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
void speed_init(speed_tracker_t* st) {
    InitializeCriticalSection(&st->lock);
    st->downloaded = 0;
    st->speed = 0;
    st->last_time = GetTickCount64();
    st->last_bytes = 0;
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

/* ===== Argument parsing ===== */
static void parse_args(options_t* opts, int argc, char** argv) {
    memset(opts, 0, sizeof(options_t));
    opts->connections = DEFAULT_CONNECTIONS;
    opts->timeout_sec = DEFAULT_TIMEOUT;
    opts->max_retries = DEFAULT_RETRY;
    opts->progress_mode = PROGRESS_BAR;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            opts->help = true; return;
        }
        else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            opts->version = true; return;
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opts->quiet = true;
            opts->progress_mode = PROGRESS_SILENT;
        }
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i+1 < argc) {
            strncpy(opts->output, argv[++i], sizeof(opts->output) - 1);
        }
        else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) && i+1 < argc) {
            strncpy(opts->dir, argv[++i], sizeof(opts->dir) - 1);
        }
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--connections") == 0) && i+1 < argc) {
            opts->connections = atoi(argv[++i]);
            if (opts->connections < 1) opts->connections = 1;
            if (opts->connections > 32) opts->connections = 32;
        }
        else if ((strcmp(argv[i], "-ua") == 0 || strcmp(argv[i], "--user-agent") == 0) && i+1 < argc) {
            strncpy(opts->user_agent, argv[++i], sizeof(opts->user_agent) - 1);
        }
        else if (strcmp(argv[i], "--referer") == 0 && i+1 < argc) {
            strncpy(opts->referer, argv[++i], sizeof(opts->referer) - 1);
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--progress") == 0) && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "bar") == 0) opts->progress_mode = PROGRESS_BAR;
            else if (strcmp(argv[i], "json") == 0) opts->progress_mode = PROGRESS_JSON;
            else if (strcmp(argv[i], "none") == 0 || strcmp(argv[i], "quiet") == 0)
                opts->progress_mode = PROGRESS_SILENT;
            else die("Unknown progress mode: %s", argv[i]);
        }
        else if (strcmp(argv[i], "--header") == 0 && i+1 < argc) {
            if (opts->extra_count < 32)
                opts->extra_headers[opts->extra_count++] = _strdup(argv[++i]);
        }
        else if (strcmp(argv[i], "--timeout") == 0 && i+1 < argc) {
            opts->timeout_sec = atoi(argv[++i]);
            if (opts->timeout_sec < 1) opts->timeout_sec = 1;
        }
        else if (strcmp(argv[i], "--retries") == 0 && i+1 < argc) {
            opts->max_retries = atoi(argv[++i]);
            if (opts->max_retries < 0) opts->max_retries = 0;
        }
        else if (argv[i][0] == '-') {
            die("Unknown option: %s. Use -h for help.", argv[i]);
        }
        else {
            if (opts->url[0] == 0)
                strncpy(opts->url, argv[i], sizeof(opts->url) - 1);
        }
    }
}

static void print_help(void) {
    printf("大侠阿木：daxiaamu.com\n");
    printf("MUDL - Multi-threaded Universal Downloader\n");
    printf("Usage: mudl [options] <URL>\n\n");
    printf("Options:\n");
    printf("  -o,  --output <FILE>      Output filename\n");
    printf("  -d,  --dir <DIR>          Output directory\n");
    printf("  -c,  --connections <N>    Connections (default %d, 1-32)\n", DEFAULT_CONNECTIONS);
    printf("  -q,  --quiet              Quiet mode\n");
    printf("  -p,  --progress <FORMAT>  Progress: bar|json|none\n");
    printf("  -ua, --user-agent <UA>    Custom User-Agent\n");
    printf("       --referer <URL>      Referer\n");
    printf("       --header <K:V>       Custom HTTP header (repeatable)\n");
    printf("       --timeout <SEC>      Timeout (default %d)\n", DEFAULT_TIMEOUT);
    printf("       --retries <N>        Retries (default %d)\n", DEFAULT_RETRY);
    printf("  -h,  --help               Show help\n");
    printf("  -V,  --version            Show version\n\n");
    printf("Examples:\n");
    printf("  mudl https://example.com/file.zip\n");
    printf("  mudl -c 8 https://example.com/large.iso\n");
    printf("  mudl -q -p json https://example.com/file.bin\n");
}
static void print_version(void) {
    printf("MUDL v%s\n", VERSION);
    printf("Multi-threaded Universal Downloader (MUDL)\n");
}

static void sig_handler(int sig) {
    (void)sig;
    g_interrupted = true;
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
