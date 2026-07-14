#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

#include "segment.h"
#include "http.h"
#include "file_io.h"
#include "oss_ticket.h"

/* Worker context passed to each thread */
typedef struct {
    int          thread_id;
    const char*  url;
    const char*  user_agent;
    const char*  referer;
    const char** extra_headers;
    int          extra_count;
    int          timeout_sec;
    const char*  resource_validator;
    const proxy_config_t* proxy;
    segment_manager_t* segmgr;
    file_t*      output_file;
    volatile bool* interrupted;
    int64_t*     global_downloaded;
    oss_ticket_t* ticket;
} worker_ctx_t;

/* Start N worker threads. Returns array of thread handles. */
HANDLE* thread_pool_start(int count, worker_ctx_t* base_ctx, int* out_count);

/* Wait for all worker threads to finish */
void thread_pool_wait(HANDLE* threads, int count);

/* Cleanup thread handles */
void thread_pool_cleanup(HANDLE* threads, int count);

/* Worker thread function */
DWORD WINAPI worker_thread_func(LPVOID param);

#endif /* THREAD_POOL_H */
