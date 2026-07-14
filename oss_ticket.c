#include "oss_ticket.h"
#include "url.h"

#include <stdio.h>
#include <string.h>

bool oss_ticket_is_check_url(const char* url) {
    return url_is_oppo_download_check(url);
}

static int resolve_locked(oss_ticket_t* ticket, char* err, int err_n) {
    http_client_t cli;
    if (http_connect(&cli, ticket->check_url, ticket->timeout_sec,
                     ticket->proxy) != 0) {
        _snprintf(err, err_n, "%s", cli.last_error);
        return -1;
    }

    char scheme[16], host[256], path[HTTP_MAX_PATH];
    int port;
    if (http_parse_url(ticket->check_url, scheme, sizeof(scheme),
                       host, sizeof(host), &port, path, sizeof(path)) != 0) {
        _snprintf(err, err_n, "Invalid downloadCheck URL");
        http_close(&cli);
        return -1;
    }

    const char* headers[33];
    int count = 0;
    headers[count++] = "userid: oplus-ota|";
    for (int i = 0; i < ticket->extra_count && count < 33; i++)
        headers[count++] = ticket->extra_headers[i];

    http_response_t resp;
    int r = http_request(&cli, HTTP_HEAD, path, NULL, NULL,
                         ticket->user_agent, ticket->referer,
                         headers, count, &resp);
    if (r != 0) {
        _snprintf(err, err_n, "%s", cli.last_error);
        http_close(&cli);
        return -1;
    }
    http_close(&cli);

    if (!resp.location[0]) {
        _snprintf(err, err_n,
                  "downloadCheck HEAD returned HTTP %d without Location",
                  resp.status_code);
        return -1;
    }
    if (!strstr(resp.location, "://")) {
        _snprintf(err, err_n, "downloadCheck returned a relative Location");
        return -1;
    }

    snprintf(ticket->current_url, sizeof(ticket->current_url), "%s",
             resp.location);
    ticket->generation++;
    return 0;
}

int oss_ticket_init(oss_ticket_t* ticket, const char* url,
                    int timeout_sec, const char* user_agent,
                    const char* referer, const char** extra_headers,
                    int extra_count, const proxy_config_t* proxy,
                    char* err, int err_n) {
    memset(ticket, 0, sizeof(*ticket));
    snprintf(ticket->current_url, sizeof(ticket->current_url), "%s", url);
    if (!oss_ticket_is_check_url(url)) return 0;

    ticket->enabled = true;
    ticket->timeout_sec = timeout_sec;
    ticket->user_agent = user_agent;
    ticket->referer = referer;
    ticket->extra_headers = extra_headers;
    ticket->extra_count = extra_count;
    ticket->proxy = proxy;
    snprintf(ticket->check_url, sizeof(ticket->check_url), "%s", url);
    InitializeCriticalSection(&ticket->lock);
    ticket->lock_initialized = true;

    EnterCriticalSection(&ticket->lock);
    int r = resolve_locked(ticket, err, err_n);
    LeaveCriticalSection(&ticket->lock);
    return r;
}

void oss_ticket_destroy(oss_ticket_t* ticket) {
    if (ticket && ticket->lock_initialized) {
        DeleteCriticalSection(&ticket->lock);
        ticket->lock_initialized = false;
    }
}

void oss_ticket_snapshot(oss_ticket_t* ticket, char* url, int url_n,
                         unsigned long* generation) {
    if (ticket->lock_initialized) EnterCriticalSection(&ticket->lock);
    snprintf(url, url_n, "%s", ticket->current_url);
    if (generation) *generation = ticket->generation;
    if (ticket->lock_initialized) LeaveCriticalSection(&ticket->lock);
}

void oss_ticket_set_current(oss_ticket_t* ticket, const char* url) {
    if (!ticket || !url) return;
    if (ticket->lock_initialized) EnterCriticalSection(&ticket->lock);
    snprintf(ticket->current_url, sizeof(ticket->current_url), "%s", url);
    if (ticket->lock_initialized) LeaveCriticalSection(&ticket->lock);
}

int oss_ticket_refresh(oss_ticket_t* ticket, unsigned long observed_generation,
                       char* err, int err_n) {
    if (!ticket || !ticket->enabled) {
        _snprintf(err, err_n, "URL has no renewable downloadCheck ticket");
        return -1;
    }

    EnterCriticalSection(&ticket->lock);
    int r = 0;
    if (ticket->generation == observed_generation)
        r = resolve_locked(ticket, err, err_n);
    LeaveCriticalSection(&ticket->lock);
    return r;
}
