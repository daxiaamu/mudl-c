#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include "http.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>


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
static int http_recv_line(http_client_t* cli, char* buf, int buf_n);

static bool winsock_inited = false;

static int socket_send_all(SOCKET fd, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static bool str_contains_i(const char* text, const char* needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return true;
    for (; *text; text++)
        if (_strnicmp(text, needle, needle_len) == 0) return true;
    return false;
}

static void trim_copy(const char* src, size_t len, char* out, int out_n) {
    while (len > 0 && (*src == ' ' || *src == '\t')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t'))
        len--;
    if (len >= (size_t)out_n) len = out_n - 1;
    memcpy(out, src, len);
    out[len] = 0;
}

static void base64_encode(const char* src, char* out, int out_n) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int len = (int)strlen(src);
    int pos = 0;
    for (int i = 0; i < len && pos < out_n - 4; i += 3) {
        int remain = len - i;
        unsigned int v = ((unsigned char)src[i]) << 16;
        if (remain > 1) v |= ((unsigned char)src[i + 1]) << 8;
        if (remain > 2) v |= (unsigned char)src[i + 2];
        out[pos++] = tab[(v >> 18) & 63];
        out[pos++] = tab[(v >> 12) & 63];
        out[pos++] = (remain > 1) ? tab[(v >> 6) & 63] : '=';
        out[pos++] = (remain > 2) ? tab[v & 63] : '=';
    }
    out[pos] = 0;
}

static bool host_matches_no_proxy(const char* host, const char* list) {
    if (!host || !host[0] || !list || !list[0]) return false;

    const char* p = list;
    while (*p) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char item[256];
        trim_copy(p, len, item, sizeof(item));

        if (strcmp(item, "*") == 0) return true;
        if (item[0] == '.') {
            const char* domain = item + 1;
            size_t host_len = strlen(host);
            size_t item_len = strlen(item);
            if (_stricmp(host, domain) == 0 ||
                (host_len > item_len &&
                 _stricmp(host + host_len - item_len, item) == 0))
                return true;
        } else if (strchr(item, '/')) {
            char ip[64];
            char* slash = strchr(item, '/');
            size_t ip_len = (size_t)(slash - item);
            if (ip_len < sizeof(ip)) {
                memcpy(ip, item, ip_len);
                ip[ip_len] = 0;
                int bits = atoi(slash + 1);
                unsigned long host_ip = inet_addr(host);
                unsigned long net_ip = inet_addr(ip);
                if (host_ip != INADDR_NONE && net_ip != INADDR_NONE &&
                    bits >= 0 && bits <= 32) {
                    uint32_t mask = bits == 0 ? 0 : htonl(0xffffffffu << (32 - bits));
                    if (((uint32_t)host_ip & mask) == ((uint32_t)net_ip & mask))
                        return true;
                }
            }
        } else if (_stricmp(host, item) == 0) {
            return true;
        }

        if (!comma) break;
        p = comma + 1;
    }

    return false;
}

static const proxy_endpoint_t* select_proxy(const proxy_config_t* cfg,
                                            const char* scheme,
                                            const char* host) {
    if (!cfg || host_matches_no_proxy(host, cfg->no_proxy))
        return NULL;
    if (_stricmp(scheme, "https") == 0 && cfg->https.enabled)
        return &cfg->https;
    if (_stricmp(scheme, "http") == 0 && cfg->http.enabled)
        return &cfg->http;
    return cfg->all.enabled ? &cfg->all : NULL;
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
    int r = socket_send_all(cli->fd, (char*)outBuffers[0].pvBuffer,
                            (int)outBuffers[0].cbBuffer);
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
                if (socket_send_all(cli->fd, (char*)outBuffers[0].pvBuffer,
                                    (int)outBuffers[0].cbBuffer) < 0) {
                    FreeContextBuffer(outBuffers[0].pvBuffer);
                    return -1;
                }
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
                if (socket_send_all(cli->fd, (char*)outBuffers[0].pvBuffer,
                                    (int)outBuffers[0].cbBuffer) < 0) {
                    FreeContextBuffer(outBuffers[0].pvBuffer);
                    return -1;
                }
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
    int sent = socket_send_all(cli->fd, msg, total);
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

int http_proxy_parse(const char* proxy, proxy_endpoint_t* out, char* err, int err_n) {
    memset(out, 0, sizeof(*out));
    if (!proxy || !proxy[0]) return 0;

    const char* p = proxy;
    const char* scheme = strstr(proxy, "://");
    if (scheme) {
        if (_strnicmp(proxy, "http://", 7) != 0) {
            _snprintf(err, err_n, "Only HTTP proxy is supported: %s", proxy);
            return -1;
        }
        p = scheme + 3;
    }

    const char* at = strchr(p, '@');
    const char* host_start = p;
    if (at) {
        char userpass[512];
        trim_copy(p, (size_t)(at - p), userpass, sizeof(userpass));
        char encoded[512];
        base64_encode(userpass, encoded, sizeof(encoded));
        _snprintf(out->auth, sizeof(out->auth), "Basic %s", encoded);
        host_start = at + 1;
    }

    const char* slash = strchr(host_start, '/');
    size_t hostport_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
    char hostport[512];
    trim_copy(host_start, hostport_len, hostport, sizeof(hostport));

    char* colon = strrchr(hostport, ':');
    out->port = 80;
    if (colon) {
        *colon = 0;
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) {
            _snprintf(err, err_n, "Invalid proxy port: %s", colon + 1);
            return -1;
        }
    }

    if (!hostport[0]) {
        _snprintf(err, err_n, "Invalid proxy host: %s", proxy);
        return -1;
    }

    strncpy(out->host, hostport, sizeof(out->host) - 1);
    out->enabled = true;
    return 0;
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

    const proxy_endpoint_t* px = select_proxy(proxy, scheme, host);
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

    /* If HTTPS, perform SSL handshake */
    if (_stricmp(scheme, "https") == 0) {
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
        _snprintf(cli->last_error, sizeof(cli->last_error),
                  "Recv error: %d", err);
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
