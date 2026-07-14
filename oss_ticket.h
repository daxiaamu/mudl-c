#ifndef OSS_TICKET_H
#define OSS_TICKET_H

#include <stdbool.h>
#include <windows.h>

#include "http.h"

typedef struct {
    bool enabled;
    bool lock_initialized;
    CRITICAL_SECTION lock;
    unsigned long generation;
    char check_url[HTTP_MAX_URL];
    char current_url[HTTP_MAX_URL];
    int timeout_sec;
    const char* user_agent;
    const char* referer;
    const char** extra_headers;
    int extra_count;
    const proxy_config_t* proxy;
} oss_ticket_t;

bool oss_ticket_is_check_url(const char* url);
int oss_ticket_init(oss_ticket_t* ticket, const char* url,
                    int timeout_sec, const char* user_agent,
                    const char* referer, const char** extra_headers,
                    int extra_count, const proxy_config_t* proxy,
                    char* err, int err_n);
void oss_ticket_destroy(oss_ticket_t* ticket);
void oss_ticket_snapshot(oss_ticket_t* ticket, char* url, int url_n,
                         unsigned long* generation);
void oss_ticket_set_current(oss_ticket_t* ticket, const char* url);
int oss_ticket_refresh(oss_ticket_t* ticket, unsigned long observed_generation,
                       char* err, int err_n);

#endif /* OSS_TICKET_H */
