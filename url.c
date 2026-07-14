#include "url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool url_is_oppo_download_check(const char* url) {
    if (!url) return false;
    const char* scheme = strstr(url, "://");
    if (!scheme || (scheme - url) != 5 || _strnicmp(url, "https", 5) != 0)
        return false;

    const char* host = scheme + 3;
    const char* path = strchr(host, '/');
    if (!path) return false;
    const char* host_end = path;
    const char* port = memchr(host, ':', (size_t)(host_end - host));
    if (port) host_end = port;

    static const char suffix[] = ".allawnos.com";
    size_t host_len = (size_t)(host_end - host);
    size_t suffix_len = sizeof(suffix) - 1;
    if (host_len <= suffix_len ||
        _strnicmp(host + host_len - suffix_len, suffix, suffix_len) != 0)
        return false;
    return _strnicmp(path, "/downloadCheck?", 15) == 0;
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

bool url_host_matches_no_proxy(const char* host, const char* list) {
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

const proxy_endpoint_t* url_select_proxy(const proxy_config_t* cfg,
                                            const char* scheme,
                                            const char* host) {
    if (!cfg || url_host_matches_no_proxy(host, cfg->no_proxy))
        return NULL;
    if (_stricmp(scheme, "https") == 0 && cfg->https.enabled)
        return &cfg->https;
    if (_stricmp(scheme, "http") == 0 && cfg->http.enabled)
        return &cfg->http;
    return cfg->all.enabled ? &cfg->all : NULL;
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
