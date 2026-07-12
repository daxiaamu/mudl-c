#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include "http.h"
#include "url.h"
#include "schannel.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>


static int http_recv_line(http_client_t* cli, char* buf, int buf_n);

static bool winsock_inited = false;

static const char* winsock_error_name(int code) {
    switch (code) {
        case WSAETIMEDOUT: return "connection timed out";
        case WSAECONNRESET: return "connection reset by peer";
        case WSAECONNREFUSED: return "connection refused";
        case WSAEHOSTUNREACH: return "host unreachable";
        case WSAENETUNREACH: return "network unreachable";
        case WSAEHOSTDOWN: return "host is down";
        case WSAENETRESET: return "network connection reset";
        case WSAECONNABORTED: return "connection aborted";
        default: return "socket error";
    }
}

static void set_socket_error(char* out, int out_n, const char* operation, int code) {
    _snprintf(out, out_n, "%s: %s (WinSock %d)",
              operation, winsock_error_name(code), code);
}

static bool str_contains_i(const char* text, const char* needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return true;
    for (; *text; text++)
        if (_strnicmp(text, needle, needle_len) == 0) return true;
    return false;
}

static int connect_with_timeout(SOCKET fd, const struct sockaddr* addr,
                                int addrlen, int timeout_sec, char* err,
                                int err_n) {
    u_long nonblock = 1;
    ioctlsocket(fd, FIONBIO, &nonblock);

    int r = connect(fd, addr, addrlen);
    if (r == 0) {
        nonblock = 0;
        ioctlsocket(fd, FIONBIO, &nonblock);
        return 0;
    }

    int wsa = WSAGetLastError();
    if (wsa != WSAEWOULDBLOCK && wsa != WSAEINPROGRESS && wsa != WSAEINVAL) {
        set_socket_error(err, err_n, "connect failed", wsa);
        nonblock = 0;
        ioctlsocket(fd, FIONBIO, &nonblock);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_sec > 0 ? timeout_sec : 30;
    tv.tv_usec = 0;

    r = select(0, NULL, &wfds, NULL, &tv);
    if (r <= 0) {
        if (r == 0)
            set_socket_error(err, err_n, "connect failed", WSAETIMEDOUT);
        else
            set_socket_error(err, err_n, "connect select failed", WSAGetLastError());
        nonblock = 0;
        ioctlsocket(fd, FIONBIO, &nonblock);
        return -1;
    }

    int so_error = 0;
    int len = sizeof(so_error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
    if (so_error != 0) {
        set_socket_error(err, err_n, "connect failed", so_error);
        nonblock = 0;
        ioctlsocket(fd, FIONBIO, &nonblock);
        return -1;
    }

    nonblock = 0;
    ioctlsocket(fd, FIONBIO, &nonblock);
    return 0;
}

static int proxy_connect_tunnel(http_client_t* cli) {
    char req[2048];
    int pos = _snprintf(req, sizeof(req),
                        "CONNECT %s:%d HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Proxy-Connection: keep-alive\r\n",
                        cli->host, cli->port, cli->host, cli->port);
    if (cli->proxy_auth[0]) {
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "Proxy-Authorization: %s\r\n", cli->proxy_auth);
    }
    pos += _snprintf(req + pos, sizeof(req) - pos, "\r\n");

    int sent = 0;
    while (sent < pos) {
        int r = send(cli->fd, req + sent, pos - sent, 0);
        if (r <= 0) {
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "Proxy CONNECT send failed: %d", WSAGetLastError());
            return -1;
        }
        sent += r;
    }

    char line[HTTP_MAX_HEADER_LINE];
    if (http_recv_line(cli, line, sizeof(line)) < 0) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "No response from proxy");
        return -1;
    }

    int code = 0;
    char ver[16];
    if (sscanf(line, "%15s %d", ver, &code) != 2 || code < 200 || code >= 300) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Proxy CONNECT failed: %s", line);
        return -1;
    }

    while (http_recv_line(cli, line, sizeof(line)) > 0) {
    }
    return 0;
}


/* ===== SChannel SSL Implementation ===== */


int http_global_init(void) {
    if (winsock_inited) return 0;
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        die("WSAStartup failed: %d", r);
        return -1;
    }
    winsock_inited = true;
    return 0;
}

void http_global_cleanup(void) {
    schannel_global_cleanup();
    if (winsock_inited) {
        WSACleanup();
        winsock_inited = false;
    }
}

int http_connect(http_client_t* cli, const char* url, int timeout_sec,
                 const proxy_config_t* proxy) {
    memset(cli, 0, sizeof(http_client_t));
    cli->timeout_sec = timeout_sec > 0 ? timeout_sec : 30;

    char scheme[16], host[256], path[HTTP_MAX_PATH];
    int port;
    if (http_parse_url(url, scheme, sizeof(scheme),
                       host, sizeof(host), &port, path, sizeof(path)) != 0) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Invalid URL: %s", url);
        return -1;
    }

    strncpy(cli->scheme, scheme, sizeof(cli->scheme) - 1);
    strncpy(cli->host, host, sizeof(cli->host) - 1);
    cli->port = port;

    const proxy_endpoint_t* px = url_select_proxy(proxy, scheme, host);
    const char* connect_host = px ? px->host : host;
    int connect_port = px ? px->port : port;
    if (px) {
        cli->proxy_active = true;
        cli->proxy_tunnel = (_stricmp(scheme, "https") == 0);
        strncpy(cli->proxy_auth, px->auth, sizeof(cli->proxy_auth) - 1);
    }

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    _snprintf(port_str, sizeof(port_str), "%d", connect_port);

    int r = getaddrinfo(connect_host, port_str, &hints, &result);
    if (r != 0 || !result) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "DNS resolution failed: %s (error %d)", connect_host, r);
        return -1;
    }

    cli->fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (cli->fd == INVALID_SOCKET) {
        freeaddrinfo(result);
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Socket creation failed: %d", WSAGetLastError());
        return -1;
    }

    char connect_error[128] = {0};
    r = connect_with_timeout(cli->fd, result->ai_addr, (int)result->ai_addrlen,
                             cli->timeout_sec, connect_error, sizeof(connect_error));
    freeaddrinfo(result);

    if (r != 0) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Connection to %s:%d failed: %s",
                  connect_host, connect_port,
                  connect_error[0] ? connect_error : "unknown error");
        closesocket(cli->fd);
        cli->fd = INVALID_SOCKET;
        return -1;
    }

    DWORD timeout_ms = (DWORD)cli->timeout_sec * 1000;
    setsockopt(cli->fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(cli->fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    cli->connected = true;

    if (cli->proxy_tunnel && proxy_connect_tunnel(cli) != 0) {
        closesocket(cli->fd);
        cli->fd = INVALID_SOCKET;
        cli->connected = false;
        return -1;
    }

    if (_stricmp(scheme, "https") == 0 && schannel_connect(cli) != 0) {
        closesocket(cli->fd);
        cli->fd = INVALID_SOCKET;
        cli->connected = false;
        return -1;
    }


    return 0;
}

static int http_send(http_client_t* cli, const char* data, int len) {
    if (cli->ssl_ctx) {
        return schannel_send(cli, data, len);
    }
    int sent = 0;
    while (sent < len) {
        int r = send(cli->fd, data + sent, len - sent, 0);
        if (r <= 0) {
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "Send error: %d", WSAGetLastError());
            return -1;
        }
        sent += r;
    }
    return sent;
}

static int http_recv_line(http_client_t* cli, char* buf, int buf_n) {
    int pos = 0;
    while (pos < buf_n - 1) {
        int r;
        if (cli->ssl_ctx) {
            r = schannel_recv(cli, buf + pos, 1);
        } else {
            r = recv(cli->fd, buf + pos, 1, 0);
        }
        if (r <= 0) return -1;
        if (buf[pos] == '\n') {
            buf[pos] = 0;
            if (pos > 0 && buf[pos-1] == '\r') buf[pos-1] = 0;
            return pos;
        }
        pos++;
    }
    buf[pos] = 0;
    return pos;
}

int http_request(http_client_t* cli, http_method_t method,
                 const char* path, const char* range_start,
                 const char* range_end, const char* ua,
                 const char* referer, const char** extra_headers,
                 int extra_count, http_response_t* resp) {
    memset(resp, 0, sizeof(http_response_t));
    cli->body_chunked = false;
    cli->chunk_done = false;
    cli->chunk_remaining = 0;

    char req[32768];
    int pos = 0;

    const char* method_str = (method == HTTP_HEAD) ? "HEAD" : "GET";
    char request_target[HTTP_MAX_URL];
    if (cli->proxy_active && !cli->proxy_tunnel && !cli->ssl_ctx) {
        bool default_port = (_stricmp(cli->scheme, "http") == 0 && cli->port == 80) ||
                            (_stricmp(cli->scheme, "https") == 0 && cli->port == 443);
        _snprintf(request_target, sizeof(request_target), "%s://%s%s%s",
                  cli->scheme, cli->host, default_port ? "" : ":", default_port ? "" : "");
        if (!default_port) {
            size_t used = strlen(request_target);
            _snprintf(request_target + used, sizeof(request_target) - used,
                      "%d", cli->port);
        }
        strncat(request_target, path, sizeof(request_target) - strlen(request_target) - 1);
    } else {
        strncpy(request_target, path, sizeof(request_target) - 1);
        request_target[sizeof(request_target) - 1] = 0;
    }

    pos += _snprintf(req + pos, sizeof(req) - pos,
                     "%s %s HTTP/1.1\r\n", method_str, request_target);
    pos += _snprintf(req + pos, sizeof(req) - pos,
                     "Host: %s\r\n", cli->host);

    if (ua && ua[0])
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "User-Agent: %s\r\n", ua);
    else
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "User-Agent: MUDM/1.0\r\n");

    pos += _snprintf(req + pos, sizeof(req) - pos, "Accept: */*\r\n");
    pos += _snprintf(req + pos, sizeof(req) - pos, "Connection: keep-alive\r\n");
    if (cli->proxy_active && !cli->proxy_tunnel && cli->proxy_auth[0]) {
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "Proxy-Authorization: %s\r\n", cli->proxy_auth);
    }

    if (referer && referer[0])
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "Referer: %s\r\n", referer);

    if (range_start && range_start[0]) {
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "Range: bytes=%s", range_start);
        if (range_end && range_end[0])
            pos += _snprintf(req + pos, sizeof(req) - pos, "-%s", range_end);
        else if (!str_ends_with(range_start, "-"))
            pos += _snprintf(req + pos, sizeof(req) - pos, "-");
        pos += _snprintf(req + pos, sizeof(req) - pos, "\r\n");
    }

    for (int i = 0; i < extra_count; i++) {
        if (extra_headers[i])
            pos += _snprintf(req + pos, sizeof(req) - pos,
                             "%s\r\n", extra_headers[i]);
    }

    pos += _snprintf(req + pos, sizeof(req) - pos, "\r\n");

    if (http_send(cli, req, pos) != pos) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Failed to send request");
        return -1;
    }

    char line[HTTP_MAX_HEADER_LINE];
    if (http_recv_line(cli, line, sizeof(line)) < 0) {
        if (!cli->last_error[0]) {
            int code = WSAGetLastError();
            if (code)
                set_socket_error(cli->last_error, sizeof(cli->last_error),
                                 "No HTTP response", code);
            else
                _snprintf(cli->last_error, sizeof(cli->last_error),
                          "No HTTP response: server closed the connection before headers");
        }
        return -1;
    }

    char http_ver[16];
    int sc;
    if (sscanf(line, "%15s %d", http_ver, &sc) != 2) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Malformed status line: %s", line);
        return -1;
    }
    resp->status_code = sc;

    int hdr_pos = 0;
    while (1) {
        int line_len = http_recv_line(cli, line, sizeof(line));
        if (line_len < 0) break;
        if (line_len >= (int)sizeof(line) - 1) {
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "HTTP header line too long");
            return -1;
        }
        if (line[0] == 0) break;

        int ll = (int)strlen(line);
        if (hdr_pos + ll + 3 < HTTP_MAX_HEADERS) {
            memcpy(resp->headers + hdr_pos, line, ll);
            hdr_pos += ll;
            resp->headers[hdr_pos++] = '\r';
            resp->headers[hdr_pos++] = '\n';
        }

        if (_strnicmp(line, "Content-Length:", 15) == 0) {
            resp->content_length = atoll(line + 15);
        }
        else if (_strnicmp(line, "Content-Range:", 14) == 0) {
            long long s, e, t;
            if (sscanf(line + 14, " bytes %lld-%lld/%lld", &s, &e, &t) == 3) {
                resp->content_range_start = s;
                resp->content_range_end = e;
                resp->content_range_total = t;
                resp->accept_ranges = true;
            }
        }
        else if (_strnicmp(line, "Accept-Ranges:", 14) == 0) {
            resp->accept_ranges = true;
        }
        else if (_strnicmp(line, "Location:", 9) == 0) {
            const char* loc = str_trim(line + 9);
            snprintf(resp->location, sizeof(resp->location), "%s", loc);
        }
        else if (_strnicmp(line, "Transfer-Encoding:", 18) == 0) {
            if (str_contains_i(line + 18, "chunked"))
                resp->is_chunked = true;
        }
        else if (_strnicmp(line, "Content-Type:", 13) == 0) {
            const char* ct = str_trim(line + 13);
            strncpy(resp->content_type, ct, sizeof(resp->content_type) - 1);
        }
        else if (_strnicmp(line, "ETag:", 5) == 0) {
            const char* value = str_trim(line + 5);
            snprintf(resp->etag, sizeof(resp->etag), "%s", value);
        }
        else if (_strnicmp(line, "Last-Modified:", 14) == 0) {
            const char* value = str_trim(line + 14);
            snprintf(resp->last_modified, sizeof(resp->last_modified), "%s", value);
        }
    }
    resp->headers[hdr_pos] = 0;
    cli->body_chunked = resp->is_chunked;
    return 0;
}

static int http_read_transport(http_client_t* cli, char* buf, int buf_size) {
    int r;
    if (cli->ssl_ctx) {
        r = schannel_recv(cli, buf, buf_size);
        if (r < 0) {
            if (!cli->last_error[0])
                _snprintf(cli->last_error, sizeof(cli->last_error),
                          "TLS receive failed: unknown SChannel error");
            return -1;
        }
        return r;
    }
    r = recv(cli->fd, buf, buf_size, 0);
    if (r < 0) {
        int err = WSAGetLastError();
        set_socket_error(cli->last_error, sizeof(cli->last_error),
                         "Receive failed", err);
        return -1;
    }
    return r;
}

int http_read_body(http_client_t* cli, char* buf, int buf_size) {
    if (cli->body_chunked)
        return http_read_body_chunked(cli, buf, buf_size, &cli->chunk_done);
    return http_read_transport(cli, buf, buf_size);
}



/* ===== Chunked Transfer-Encoding support ===== */

/* Read chunk header: returns chunk size, or 0 for last chunk, -1 on error */
static int http_recv_byte(http_client_t* cli) {
    char b;
    int r = http_read_transport(cli, &b, 1);
    if (r <= 0) return -1;
    return (unsigned char)b;
}

int http_read_chunk_size(http_client_t* cli) {
    char line[64];
    int pos = 0;
    while (pos < (int)sizeof(line) - 1) {
        int b = http_recv_byte(cli);
        if (b < 0) return -1;
        line[pos] = (char)b;
        if (line[pos] == '\n') {
            line[pos] = 0;
            /* Trim CR */
            if (pos > 0 && line[pos-1] == '\r') line[pos-1] = 0;
            /* Parse hex size (skip extensions after ;) */
            char* ext = strchr(line, ';');
            if (ext) *ext = 0;
            char* end = NULL;
            unsigned long size = strtoul(line, &end, 16);
            if (end == line || *end != 0 || size > INT_MAX) return -1;
            return (int)size;
        }
        pos++;
    }
    return -1;
}

/* Read chunk trailer (after last chunk, before next response).
   Returns 0 on success, -1 on error. */
int http_read_chunk_trailer(http_client_t* cli) {
    char line[256];
    while (1) {
        if (http_recv_line(cli, line, sizeof(line)) < 0) return -1;
        if (line[0] == 0) return 0;
    }
}

/* Read body with chunked transfer-encoding support.
   Compatible with http_read_body API: returns bytes read, 0 = EOF, -1 = error. */
int http_read_body_chunked(http_client_t* cli, char* buf, int buf_size,
                           bool* last_chunk) {
    if (*last_chunk) return 0;

    if (cli->chunk_remaining == 0) {
        int chunk_size = http_read_chunk_size(cli);
        if (chunk_size < 0) return -1;
        if (chunk_size == 0) {
            if (http_read_chunk_trailer(cli) != 0) return -1;
            *last_chunk = true;
            return 0;
        }
        cli->chunk_remaining = chunk_size;
    }

    int to_read = cli->chunk_remaining < buf_size
                ? (int)cli->chunk_remaining : buf_size;
    int got = http_read_transport(cli, buf, to_read);
    if (got <= 0) {
        if (got == 0)
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "Unexpected EOF in chunked response");
        return -1;
    }
    cli->chunk_remaining -= got;

    if (cli->chunk_remaining == 0) {
        int cr = http_recv_byte(cli);
        int lf = http_recv_byte(cli);
        if (cr != '\r' || lf != '\n') return -1;
    }

    return got;
}
void http_close(http_client_t* cli) {
    if (cli->ssl_ctx) {
        schannel_close(cli);
    }
    if (cli->fd != INVALID_SOCKET) {
        shutdown(cli->fd, SD_BOTH);
        closesocket(cli->fd);
    }
    cli->fd = INVALID_SOCKET;
    cli->connected = false;
}
