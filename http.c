#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include "http.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ===== SChannel SSL support ===== */
#include <sspi.h>
#include <schnlsp.h>

#ifndef SP_PROT_TLS1_0_CLIENT
#define SP_PROT_TLS1_0_CLIENT 0x00000080
#endif

#ifndef SP_PROT_TLS1_1_CLIENT
#define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif

#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif

typedef struct {
    bool     enabled;       /* is this connection using SSL? */
    CredHandle hCred;       /* credential handle */
    CtxtHandle hCtx;        /* security context */
    SecPkgContext_StreamSizes sizes;
    bool     handshake_done;
    char     send_buf[65536];
    int      send_len;
    char     recv_buf[65536];
    int      recv_len;
    int      recv_pos;
    char     extra_buf[65536];
    int      extra_len;
} ssl_t;

/* Global SChannel credential (acquired once) */
static CredHandle g_ssl_cred;
static bool g_ssl_inited = false;

static int ssl_global_init(void);
static int ssl_connect(http_client_t* cli, ssl_t* ssl);
static int ssl_send(http_client_t* cli, ssl_t* ssl, const char* data, int len);
static int ssl_recv(http_client_t* cli, ssl_t* ssl, char* buf, int buf_size);
static void ssl_close(ssl_t* ssl);

static bool winsock_inited = false;

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
        _snprintf(err, err_n, "connect failed: %d", wsa);
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
        _snprintf(err, err_n, r == 0 ? "connect timed out" : "connect select failed: %d",
                  WSAGetLastError());
        nonblock = 0;
        ioctlsocket(fd, FIONBIO, &nonblock);
        return -1;
    }

    int so_error = 0;
    int len = sizeof(so_error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
    if (so_error != 0) {
        _snprintf(err, err_n, "connect failed: %d", so_error);
        nonblock = 0;
        ioctlsocket(fd, FIONBIO, &nonblock);
        return -1;
    }

    nonblock = 0;
    ioctlsocket(fd, FIONBIO, &nonblock);
    return 0;
}


/* ===== SChannel SSL Implementation ===== */

static int ssl_global_init(void) {
    if (g_ssl_inited) return 0;

    SCHANNEL_CRED cred = {0};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = 0; /* Use OS defaults, including TLS 1.3 where available. */
    cred.dwFlags = 0;

    SECURITY_STATUS s = AcquireCredentialsHandleA(
        NULL, (LPSTR)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
        NULL, &cred, NULL, NULL, &g_ssl_cred, NULL);
    if (s != SEC_E_OK) {
        trace("AcquireCredentialsHandle failed: 0x%08x", s);
        return -1;
    }
    g_ssl_inited = true;
    return 0;
}

static int ssl_connect(http_client_t* cli, ssl_t* ssl) {
    memset(ssl, 0, sizeof(ssl_t));
    ssl->enabled = true;
    if (!g_ssl_inited && ssl_global_init() != 0) return -1;

    /* SChannel uses this as the server name/SNI, so keep it host-only. */
    char target[512];
    snprintf(target, sizeof(target), "%s", cli->host);

    SecBuffer outBuffers[1];
    SecBufferDesc outDesc;
    outDesc.ulVersion = SECBUFFER_VERSION;
    outDesc.cBuffers = 1;
    outDesc.pBuffers = outBuffers;
    outBuffers[0].pvBuffer = NULL;
    outBuffers[0].cbBuffer = 0;
    outBuffers[0].BufferType = SECBUFFER_TOKEN;

    ULONG contextAttr = 0;
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT
                | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY
                | ISC_REQ_STREAM;

    SECURITY_STATUS s = InitializeSecurityContextA(
        &g_ssl_cred, NULL, target, flags, 0, 0,
        NULL, 0, &ssl->hCtx, &outDesc, &contextAttr, NULL);

    trace("SSL init result: 0x%08x, token len=%lu", s, outBuffers[0].cbBuffer);
    if (s != SEC_I_CONTINUE_NEEDED) {
        trace("InitializeSecurityContext failed: 0x%08x", s);
        return -1;
    }

    /* Send initial handshake data */
    int r = send(cli->fd, (char*)outBuffers[0].pvBuffer, outBuffers[0].cbBuffer, 0);
    FreeContextBuffer(outBuffers[0].pvBuffer);
    if (r <= 0) { trace("SSL handshake send failed"); return -1; }

    /* Handshake loop */
    int max_rounds = 20;
    char buf[65536];
    int buf_pos = 0;
    int buf_len = 0;
    while (max_rounds-- > 0) {
        /* Receive server response - handle fragmentation */
        int bytes;
        if (buf_pos >= buf_len) {
            bytes = recv(cli->fd, buf, sizeof(buf), 0);
            if (bytes <= 0) { trace("SSL handshake recv failed: %d", bytes); return -1; }
            buf_pos = 0;
            buf_len = bytes;
        } else {
            bytes = buf_len - buf_pos;
        }

        SecBuffer inBuffers[2];
        SecBufferDesc inDesc;
        inDesc.ulVersion = SECBUFFER_VERSION;
        inDesc.cBuffers = 2;
        inDesc.pBuffers = inBuffers;
        inBuffers[0].pvBuffer = buf + buf_pos;
        inBuffers[0].cbBuffer = bytes;
        inBuffers[0].BufferType = SECBUFFER_TOKEN;
        inBuffers[1].pvBuffer = NULL;
        inBuffers[1].cbBuffer = 0;
        inBuffers[1].BufferType = SECBUFFER_EMPTY;

        outBuffers[0].pvBuffer = NULL;
        outBuffers[0].cbBuffer = 0;
        outBuffers[0].BufferType = SECBUFFER_TOKEN;

        s = InitializeSecurityContextA(
            &g_ssl_cred, &ssl->hCtx, target, flags, 0, 0,
            &inDesc, 0, NULL, &outDesc, &contextAttr, NULL);

        if (s == SEC_E_OK) {
            /* Handshake complete - send final token if any */
            if (outBuffers[0].cbBuffer > 0 && outBuffers[0].pvBuffer) {
                send(cli->fd, (char*)outBuffers[0].pvBuffer, outBuffers[0].cbBuffer, 0);
                FreeContextBuffer(outBuffers[0].pvBuffer);
            }
            trace("SSL handshake complete, buf_pos=%d buf_len=%d", buf_pos, buf_len);
            /* Check for extra encrypted data in input buffers */
            for (int bi = 0; bi < 2; bi++) {
                if (inBuffers[bi].BufferType == SECBUFFER_EXTRA) {
                    int extra = (int)inBuffers[bi].cbBuffer;
                                    if (extra > 0 && extra <= (int)sizeof(ssl->extra_buf)) {
                        memcpy(ssl->extra_buf, inBuffers[bi].pvBuffer, extra);
                        ssl->extra_len = extra;
                    }
                }
            }
            ssl->handshake_done = true;
            QueryContextAttributesA(&ssl->hCtx, SECPKG_ATTR_STREAM_SIZES, &ssl->sizes);
            trace("SSL handshake complete with %s", cli->host);
            return 0;
        }
        else if (s == SEC_I_CONTINUE_NEEDED) {
            /* Check how much data SChannel actually consumed */
            int consumed = bytes;
            for (int bi = 0; bi < 2; bi++) {
                if (inBuffers[bi].BufferType == SECBUFFER_EXTRA) {
                    consumed = bytes - (int)inBuffers[bi].cbBuffer;
                    break;
                }
            }
            buf_pos += consumed;
            if (outBuffers[0].cbBuffer > 0 && outBuffers[0].pvBuffer) {
                send(cli->fd, (char*)outBuffers[0].pvBuffer, outBuffers[0].cbBuffer, 0);
                FreeContextBuffer(outBuffers[0].pvBuffer);
            }
            continue;
        }
        else if (s == SEC_E_INCOMPLETE_MESSAGE) {
            /* Need more data */
            int more = recv(cli->fd, buf + buf_len, sizeof(buf) - buf_len, 0);
            if (more <= 0) { trace("SSL: incomplete message, recv failed"); return -1; }
            buf_len += more;
            continue;
        }
        else {
            trace("SSL handshake failed: 0x%08x", s);
            return -1;
        }
    }

    trace("SSL handshake timed out");
    return -1;
}

static int ssl_send(http_client_t* cli, ssl_t* ssl, const char* data, int len) {
    if (!ssl->handshake_done) return -1;
    trace("ssl_send: len=%d, first=%.80s", len, data);

    SecBuffer buffers[4];
    SecBufferDesc desc;
    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = buffers;

    /* Allocate message buffer */
    int msg_size = ssl->sizes.cbHeader + len + ssl->sizes.cbTrailer;
    char* msg = (char*)malloc(msg_size);
    if (!msg) return -1;

    memcpy(msg + ssl->sizes.cbHeader, data, len);

    buffers[0].pvBuffer = msg;
    buffers[0].cbBuffer = ssl->sizes.cbHeader;
    buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    buffers[1].pvBuffer = msg + ssl->sizes.cbHeader;
    buffers[1].cbBuffer = len;
    buffers[1].BufferType = SECBUFFER_DATA;
    buffers[2].pvBuffer = msg + ssl->sizes.cbHeader + len;
    buffers[2].cbBuffer = ssl->sizes.cbTrailer;
    buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    buffers[3].pvBuffer = SECBUFFER_EMPTY;
    buffers[3].cbBuffer = 0;
    buffers[3].BufferType = SECBUFFER_EMPTY;

    SECURITY_STATUS s = EncryptMessage(&ssl->hCtx, 0, &desc, 0);
    if (s != SEC_E_OK) {
        free(msg);
        trace("EncryptMessage failed: 0x%08x", s);
        return -1;
    }

    int total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
    int sent = send(cli->fd, msg, total, 0);
    free(msg);

    if (sent <= 0) return -1;
    return len;  /* Return original data length, not encrypted length */
}

static int ssl_recv(http_client_t* cli, ssl_t* ssl, char* buf, int buf_size) {
    if (!ssl->handshake_done) return -1;

    /* If we have buffered decrypted data, use it first */
    if (ssl->recv_pos < ssl->recv_len) {
        int avail = ssl->recv_len - ssl->recv_pos;
        int to_copy = (avail < buf_size) ? avail : buf_size;
        memcpy(buf, ssl->recv_buf + ssl->recv_pos, to_copy);
        ssl->recv_pos += to_copy;
        return to_copy;
    }

    /* Use extra data from handshake first, then read from socket */
    char enc_buf[65536];
    int bytes;
    if (ssl->extra_len > 0) {
        bytes = ssl->extra_len;
        if (bytes > (int)sizeof(enc_buf)) bytes = (int)sizeof(enc_buf);
        memcpy(enc_buf, ssl->extra_buf, bytes);
        ssl->extra_len = 0;
        trace("ssl_recv: using extra data from handshake: %d bytes", bytes);
    } else {
        bytes = recv(cli->fd, enc_buf, sizeof(enc_buf), 0);
        trace("ssl_recv: raw recv=%d", bytes);
        if (bytes <= 0) return bytes;
    }

    /* Decrypt */
    SecBuffer buffers[4];
    SecBufferDesc desc;
    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = buffers;
    buffers[0].pvBuffer = enc_buf;
    buffers[0].cbBuffer = bytes;
    buffers[0].BufferType = SECBUFFER_DATA;
    buffers[1].pvBuffer = NULL;
    buffers[1].cbBuffer = 0;
    buffers[1].BufferType = SECBUFFER_EMPTY;
    buffers[2].pvBuffer = NULL;
    buffers[2].cbBuffer = 0;
    buffers[2].BufferType = SECBUFFER_EMPTY;
    buffers[3].pvBuffer = NULL;
    buffers[3].cbBuffer = 0;
    buffers[3].BufferType = SECBUFFER_EMPTY;


        trace("ssl_recv: DecryptMessage input=%d bytes", bytes);
    SECURITY_STATUS s = DecryptMessage(&ssl->hCtx, &desc, 0, NULL);
    trace("ssl_recv: DecryptMessage result=0x%08x", s);

    /* Handle incomplete message: retry with more data */
    {
        int retries = 20;
        while (s == SEC_E_INCOMPLETE_MESSAGE && retries-- > 0) {
            int more = recv(cli->fd, enc_buf + bytes, (int)sizeof(enc_buf) - bytes, 0);
            if (more <= 0) {
                trace("ssl_recv: incomplete, recv failed: %d", more);
                return -1;
            }
            bytes += more;
                        memset(buffers, 0, sizeof(buffers));
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;
            buffers[0].pvBuffer = enc_buf;
            buffers[0].cbBuffer = bytes;
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            s = DecryptMessage(&ssl->hCtx, &desc, 0, NULL);
                    }
        if (s == SEC_E_INCOMPLETE_MESSAGE) {
            trace("ssl_recv: too many retries for incomplete message");
            return -1;
        }
        if (s != SEC_E_OK) {
            trace("DecryptMessage failed: 0x%08x", s);
            return -1;
        }
    }

    /* Save any remaining encrypted data for next call */
    for (int i = 0; i < 4; i++) {
        if (buffers[i].BufferType == SECBUFFER_EXTRA && buffers[i].cbBuffer > 0) {
            int extra = (int)buffers[i].cbBuffer;
            if (extra <= (int)sizeof(ssl->extra_buf)) {
                memcpy(ssl->extra_buf, buffers[i].pvBuffer, extra);
                ssl->extra_len = extra;
                trace("ssl_recv: saved %d bytes extra encrypted data", extra);
            }
        }
    }

    /* Find decrypted data buffer */
    for (int i = 0; i < 4; i++) {
        if (buffers[i].BufferType == SECBUFFER_DATA) {
            int avail = ((int)buffers[i].cbBuffer < buf_size) ? (int)buffers[i].cbBuffer : buf_size;
            memcpy(buf, buffers[i].pvBuffer, avail);
            if ((int)buffers[i].cbBuffer > buf_size) {
                ssl->recv_len = (int)buffers[i].cbBuffer;
                ssl->recv_pos = buf_size;
                memcpy(ssl->recv_buf, buffers[i].pvBuffer, ssl->recv_len);
            }
            return avail;
        }
    }
    return 0;
}

static void ssl_close(ssl_t* ssl) {
    if (ssl->hCtx.dwLower || ssl->hCtx.dwUpper) {
        DeleteSecurityContext(&ssl->hCtx);
    }
    memset(ssl, 0, sizeof(ssl_t));
}

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
    if (winsock_inited) {
        WSACleanup();
        winsock_inited = false;
    }
}

int http_parse_url(const char* url, char* scheme, int scheme_n,
                   char* host, int host_n, int* port,
                   char* path, int path_n) {
    const char* colon = strstr(url, "://");
    if (!colon) return -1;

    size_t slen = colon - url;
    if (slen >= (size_t)scheme_n) slen = scheme_n - 1;
    memcpy(scheme, url, slen);
    scheme[slen] = 0;

    *port = (_stricmp(scheme, "https") == 0) ? 443 : 80;

    const char* h = colon + 3;
    const char* path_start = strchr(h, '/');
    const char* port_colon = strchr(h, ':');

    if (port_colon && (!path_start || port_colon < path_start)) {
        size_t hlen = port_colon - h;
        if (hlen >= (size_t)host_n) hlen = host_n - 1;
        memcpy(host, h, hlen);
        host[hlen] = 0;
        *port = atoi(port_colon + 1);
        h = port_colon + 1;
        path_start = strchr(h, '/');
    } else {
        size_t hlen = path_start ? (size_t)(path_start - h) : strlen(h);
        if (hlen >= (size_t)host_n) hlen = host_n - 1;
        memcpy(host, h, hlen);
        host[hlen] = 0;
    }

    if (path_start) {
        strncpy(path, path_start, path_n - 1);
    } else {
        strncpy(path, "/", path_n - 1);
    }
    return 0;
}

int http_connect(http_client_t* cli, const char* url, int timeout_sec) {
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

    strncpy(cli->host, host, sizeof(cli->host) - 1);
    cli->port = port;

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    _snprintf(port_str, sizeof(port_str), "%d", port);

    int r = getaddrinfo(host, port_str, &hints, &result);
    if (r != 0 || !result) {
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "DNS resolution failed: %s (error %d)", host, r);
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
                  host, port, connect_error[0] ? connect_error : "unknown error");
        closesocket(cli->fd);
        cli->fd = INVALID_SOCKET;
        return -1;
    }

    DWORD timeout_ms = (DWORD)cli->timeout_sec * 1000;
    setsockopt(cli->fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(cli->fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    cli->connected = true;

    /* If HTTPS, perform SSL handshake */
    if (port == 443) {
        ssl_t* ssl = (ssl_t*)calloc(1, sizeof(ssl_t));
        if (!ssl) {
            _snprintf(cli->last_error, sizeof(cli->last_error), "Out of memory for SSL");
            closesocket(cli->fd);
            cli->fd = INVALID_SOCKET;
            cli->connected = false;
            return -1;
        }
        if (ssl_connect(cli, ssl) != 0) {
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "TLS handshake failed with %s:%d using Windows SChannel. "
                      "Check system TLS/certificate settings, network interception, "
                      "or try another HTTPS backend.",
                      host, port);
            free(ssl);
            closesocket(cli->fd);
            cli->fd = INVALID_SOCKET;
            cli->connected = false;
            return -1;
        }
        cli->ssl_ctx = ssl;
    }

    return 0;
}

static int http_send(http_client_t* cli, const char* data, int len) {
    if (cli->ssl_ctx) {
        return ssl_send(cli, (ssl_t*)cli->ssl_ctx, data, len);
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
            r = ssl_recv(cli, (ssl_t*)cli->ssl_ctx, buf + pos, 1);
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

    char req[32768];
    int pos = 0;

    const char* method_str = (method == HTTP_HEAD) ? "HEAD" : "GET";
    pos += _snprintf(req + pos, sizeof(req) - pos,
                     "%s %s HTTP/1.1\r\n", method_str, path);
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

    if (referer && referer[0])
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "Referer: %s\r\n", referer);

    if (range_start && range_start[0]) {
        pos += _snprintf(req + pos, sizeof(req) - pos,
                         "Range: bytes=%s", range_start);
        if (range_end && range_end[0])
            pos += _snprintf(req + pos, sizeof(req) - pos, "-%s", range_end);
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
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "No response from server");
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
            if (strstr(line + 18, "chunked"))
                resp->is_chunked = true;
        }
        else if (_strnicmp(line, "Content-Type:", 13) == 0) {
            const char* ct = str_trim(line + 13);
            strncpy(resp->content_type, ct, sizeof(resp->content_type) - 1);
        }
    }
    resp->headers[hdr_pos] = 0;
    return 0;
}

int http_read_body(http_client_t* cli, char* buf, int buf_size) {
    int r;
    if (cli->ssl_ctx) {
        r = ssl_recv(cli, (ssl_t*)cli->ssl_ctx, buf, buf_size);
        if (r < 0) {
            _snprintf(cli->last_error, sizeof(cli->last_error), "SSL recv error");
            return -1;
        }
        return r;
    }
    r = recv(cli->fd, buf, buf_size, 0);
    if (r < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT)
            return 0;
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Recv error: %d", err);
        return -1;
    }
    return r;
}



/* ===== Chunked Transfer-Encoding support ===== */

/* Read chunk header: returns chunk size, or 0 for last chunk, -1 on error */
static int http_recv_byte(http_client_t* cli) {
    char b;
    int r;
    if (cli->ssl_ctx) {
        r = ssl_recv(cli, (ssl_t*)cli->ssl_ctx, &b, 1);
    } else {
        r = recv(cli->fd, &b, 1, 0);
    }
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
            long size = strtol(line, NULL, 16);
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
        int pos = 0;
        while (pos < (int)sizeof(line) - 1) {
            int r = recv(cli->fd, line + pos, 1, 0);
            if (r <= 0) return -1;
            if (line[pos] == '\n') {
                line[pos] = 0;
                if (pos > 0 && line[pos-1] == '\r') line[pos-1] = 0;
                if (line[0] == 0) return 0;  /* empty line = end of trailers */
                break;
            }
            pos++;
        }
    }
    return 0;
}

/* Read body with chunked transfer-encoding support.
   Compatible with http_read_body API: returns bytes read, 0 = EOF, -1 = error. */
int http_read_body_chunked(http_client_t* cli, char* buf, int buf_size,
                           bool* last_chunk) {
    if (*last_chunk) return 0;

    int chunk_size = http_read_chunk_size(cli);
    if (chunk_size < 0) return -1;
    if (chunk_size == 0) {
        /* Last chunk: read trailers */
        http_read_chunk_trailer(cli);
        *last_chunk = true;
        return 0;
    }

    /* Read chunk data */
    int to_read = chunk_size;
    if (to_read > buf_size) to_read = buf_size;
    int pos = 0;
    while (pos < to_read) {
        int r = recv(cli->fd, buf + pos, to_read - pos, 0);
        if (r <= 0) return -1;
        pos += r;
    }

    /* Read trailing CRLF after chunk data */
    if (http_recv_byte(cli) < 0 || http_recv_byte(cli) < 0) return -1;

    return pos;
}
void http_close(http_client_t* cli) {
    if (cli->ssl_ctx) {
        ssl_close((ssl_t*)cli->ssl_ctx);
        free(cli->ssl_ctx);
        cli->ssl_ctx = NULL;
    }
    if (cli->fd != INVALID_SOCKET) {
        shutdown(cli->fd, SD_BOTH);
        closesocket(cli->fd);
    }
    cli->fd = INVALID_SOCKET;
    cli->connected = false;
}
