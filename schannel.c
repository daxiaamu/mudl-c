#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include "schannel.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    SECURITY_STATUS last_status;
    int      last_wsa_error;
} ssl_t;

/* Global SChannel credential (acquired once) */
static CredHandle g_ssl_cred;
static bool g_ssl_inited = false;

static int ssl_global_init(void);
static int ssl_connect(http_client_t* cli, ssl_t* ssl);
static int ssl_send(http_client_t* cli, ssl_t* ssl, const char* data, int len);
static int ssl_recv(http_client_t* cli, ssl_t* ssl, char* buf, int buf_size);
static void ssl_close(ssl_t* ssl);

static const char* winsock_error_name(int code) {
    switch (code) {
        case WSAETIMEDOUT: return "connection timed out";
        case WSAECONNRESET: return "connection reset by peer";
        case WSAECONNREFUSED: return "connection refused";
        case WSAEHOSTUNREACH: return "host unreachable";
        case WSAENETUNREACH: return "network unreachable";
        default: return "socket error";
    }
}

static const char* schannel_error_name(SECURITY_STATUS status) {
    switch (status) {
        case SEC_E_UNTRUSTED_ROOT: return "certificate chain is not trusted";
        case SEC_E_CERT_EXPIRED: return "certificate is expired or not yet valid";
        case SEC_E_WRONG_PRINCIPAL: return "certificate hostname mismatch";
        case SEC_E_ILLEGAL_MESSAGE: return "peer rejected the TLS protocol or handshake";
        case SEC_E_ALGORITHM_MISMATCH: return "no compatible TLS algorithm or protocol";
        case SEC_E_CERT_UNKNOWN: return "certificate validation failed";
        case SEC_E_INCOMPLETE_MESSAGE: return "incomplete TLS record";
        default: return "SChannel handshake error";
    }
}

static void set_socket_error(char* out, int out_n, const char* operation, int code) {
    _snprintf(out, out_n, "%s: %s (WinSock %d)",
              operation, winsock_error_name(code), code);
}

static int socket_send_all(SOCKET fd, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

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
        ssl->last_status = s;
        trace("InitializeSecurityContext failed: 0x%08x", s);
        return -1;
    }

    /* Send initial handshake data */
    int r = socket_send_all(cli->fd, (char*)outBuffers[0].pvBuffer,
                            (int)outBuffers[0].cbBuffer);
    FreeContextBuffer(outBuffers[0].pvBuffer);
    if (r <= 0) {
        ssl->last_wsa_error = WSAGetLastError();
        trace("SSL handshake send failed");
        return -1;
    }

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
            if (bytes <= 0) {
                ssl->last_wsa_error = bytes == 0 ? WSAECONNRESET : WSAGetLastError();
                trace("SSL handshake recv failed: %d", bytes);
                return -1;
            }
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
            ssl->last_status = s;
            trace("SSL handshake failed: 0x%08x", s);
            return -1;
        }
    }

    ssl->last_wsa_error = WSAETIMEDOUT;
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
                int code = more == 0 ? WSAECONNRESET : WSAGetLastError();
                set_socket_error(cli->last_error, sizeof(cli->last_error),
                                 "TLS receive failed", code);
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
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "TLS receive failed: incomplete record (SChannel 0x%08lx)",
                      (unsigned long)s);
            trace("ssl_recv: too many retries for incomplete message");
            return -1;
        }
        if (s != SEC_E_OK) {
            _snprintf(cli->last_error, sizeof(cli->last_error),
                      "TLS receive failed: %s (SChannel 0x%08lx)",
                      schannel_error_name(s), (unsigned long)s);
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

int schannel_connect(http_client_t* client) {
    ssl_t* ssl = (ssl_t*)calloc(1, sizeof(ssl_t));
    if (!ssl) {
        _snprintf(client->last_error, sizeof(client->last_error),
                  "Out of memory for SChannel");
        return -1;
    }

    if (ssl_connect(client, ssl) != 0) {
        if (ssl->last_status != SEC_E_OK) {
            _snprintf(client->last_error, sizeof(client->last_error),
                      "TLS handshake failed with %s:%d: %s "
                      "(SChannel 0x%08lx). Check the Windows certificate store "
                      "and enabled TLS protocols.", client->host, client->port,
                      schannel_error_name(ssl->last_status),
                      (unsigned long)ssl->last_status);
        } else if (ssl->last_wsa_error) {
            _snprintf(client->last_error, sizeof(client->last_error),
                      "TLS handshake transport failed with %s:%d: %s "
                      "(WinSock %d)", client->host, client->port,
                      winsock_error_name(ssl->last_wsa_error),
                      ssl->last_wsa_error);
        } else {
            _snprintf(client->last_error, sizeof(client->last_error),
                      "TLS handshake failed with %s:%d using Windows SChannel",
                      client->host, client->port);
        }
        ssl_close(ssl);
        free(ssl);
        return -1;
    }

    client->ssl_ctx = ssl;
    return 0;
}

int schannel_send(http_client_t* client, const char* data, int length) {
    return ssl_send(client, (ssl_t*)client->ssl_ctx, data, length);
}

int schannel_recv(http_client_t* client, char* buffer, int buffer_size) {
    return ssl_recv(client, (ssl_t*)client->ssl_ctx, buffer, buffer_size);
}

void schannel_close(http_client_t* client) {
    if (!client || !client->ssl_ctx) return;
    ssl_close((ssl_t*)client->ssl_ctx);
    free(client->ssl_ctx);
    client->ssl_ctx = NULL;
}

void schannel_global_cleanup(void) {
    if (!g_ssl_inited) return;
    FreeCredentialsHandle(&g_ssl_cred);
    memset(&g_ssl_cred, 0, sizeof(g_ssl_cred));
    g_ssl_inited = false;
}
