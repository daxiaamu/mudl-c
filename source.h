#ifndef SOURCE_H
#define SOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

#include "http.h"
#include "oss_ticket.h"

#define MAX_DOWNLOAD_SOURCES 16
#define SOURCE_SAMPLE_SIZE 65536

typedef struct {
    char url[HTTP_MAX_URL];
    char path[HTTP_MAX_PATH];
    char validator[320];
    int64_t file_size;
    uint32_t first_crc32;
    uint32_t last_crc32;
    int first_length;
    int last_length;
    oss_ticket_t* ticket;
    volatile LONG disabled;
    volatile LONG failures;
} download_source_t;

int source_probe(download_source_t* source, const char* url,
                 int timeout_sec, const char* user_agent,
                 const char* referer, const char** extra_headers,
                 int extra_count, const proxy_config_t* proxy,
                 char* err, int err_n);
bool source_matches(const download_source_t* source,
                    const download_source_t* reference);

#endif /* SOURCE_H */
