#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stdbool.h>

/* winsock2.h MUST come before windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SECURITY_WIN32
#include <sspi.h>

#define HTTP_BUF_SIZE 65536
#define HTTP_MAX_HEADERS 16384
#define HTTP_MAX_URL 8192
#define HTTP_MAX_PATH 8192
#define HTTP_MAX_HEADER_LINE 8192

#define HTTP_OK               200
#define HTTP_PARTIAL_CONTENT  206
#define HTTP_REDIRECT         302
#define HTTP_RANGE_NOT_SATIS  416

typedef enum {
    HTTP_GET,
    HTTP_HEAD,
} http_method_t;

typedef struct {
    int         status_code;
    int64_t     content_length;
    int64_t     content_range_start;
    int64_t     content_range_end;
    int64_t     content_range_total;
    bool        accept_ranges;
    char        location[HTTP_MAX_URL];
    char        content_type[128];
    char        etag[256];
    char        last_modified[128];
    char        headers[HTTP_MAX_HEADERS];
    bool        is_chunked;
} http_response_t;

typedef struct {
    bool        enabled;
    char        host[256];
    int         port;
    char        auth[512];
} proxy_endpoint_t;

typedef struct {
    proxy_endpoint_t all;
    proxy_endpoint_t http;
    proxy_endpoint_t https;
    char        no_proxy[1024];
} proxy_config_t;

typedef struct {
    SOCKET      fd;
    char        scheme[16];
    char        host[256];
    int         port;
    bool        proxy_active;
    bool        proxy_tunnel;
    char        proxy_auth[512];
    bool        connected;
    int         timeout_sec;
    char        last_error[256];
    void*       ssl_ctx;       /* SChannel SSL context, NULL if plain HTTP */
    bool        body_chunked;
    bool        chunk_done;
    int64_t     chunk_remaining;
} http_client_t;

int http_global_init(void);
void http_global_cleanup(void);
int http_proxy_parse(const char* proxy, proxy_endpoint_t* out, char* err, int err_n);
int http_connect(http_client_t* cli, const char* url, int timeout_sec,
                 const proxy_config_t* proxy);
int http_request(http_client_t* cli, http_method_t method,
                 const char* path, const char* range_start,
                 const char* range_end, const char* ua,
                 const char* referer, const char** extra_headers,
                 int extra_count, http_response_t* resp);
int http_read_body(http_client_t* cli, char* buf, int buf_size);
void http_close(http_client_t* cli);
int http_read_chunk_size(http_client_t* cli);
int http_read_chunk_trailer(http_client_t* cli);
int http_read_body_chunked(http_client_t* cli, char* buf, int buf_size,
                           bool* last_chunk);
int http_parse_url(const char* url, char* scheme, int scheme_n,
                   char* host, int host_n, int* port,
                   char* path, int path_n);

#endif /* HTTP_H */
