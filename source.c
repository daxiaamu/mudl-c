#include "source.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void response_validator(const http_response_t* resp,
                               char* out, int out_n) {
    out[0] = 0;
    if (resp->etag[0])
        snprintf(out, out_n, "etag:%s", resp->etag);
    else if (resp->last_modified[0])
        snprintf(out, out_n, "last-modified:%s", resp->last_modified);
}

static int fetch_sample(char* url, int url_n, int64_t start, int64_t end,
                        int64_t expected_size, int timeout_sec,
                        const char* user_agent, const char* referer,
                        const char** extra_headers, int extra_count,
                        const proxy_config_t* proxy, const char* interface_name,
                        uint32_t* crc,
                        int* sample_length, char* validator, int validator_n,
                        char* path_out, int path_n, int64_t* total_size,
                        char* err, int err_n) {
    char current[HTTP_MAX_URL];
    snprintf(current, sizeof(current), "%s", url);

    for (int redirects = 0; redirects <= 5; redirects++) {
        char scheme[16], host[256], path[HTTP_MAX_PATH];
        int port;
        if (http_parse_url(current, scheme, sizeof(scheme), host, sizeof(host),
                           &port, path, sizeof(path)) != 0) {
            snprintf(err, err_n, "invalid URL");
            return -1;
        }

        http_client_t http;
        if (http_connect(&http, current, timeout_sec, proxy,
                         interface_name) != 0) {
            snprintf(err, err_n, "%s", http.last_error);
            return -1;
        }

        char range_start[32], range_end[32];
        snprintf(range_start, sizeof(range_start), "%lld", (long long)start);
        snprintf(range_end, sizeof(range_end), "%lld", (long long)end);
        http_response_t resp;
        int r = http_request(&http, HTTP_GET, path, range_start, range_end,
                             user_agent, referer, extra_headers, extra_count,
                             &resp);
        if (r != 0) {
            snprintf(err, err_n, "%s", http.last_error);
            http_close(&http);
            return -1;
        }

        if ((resp.status_code == 301 || resp.status_code == 302 ||
             resp.status_code == 307 || resp.status_code == 308) &&
            resp.location[0] && redirects < 5) {
            snprintf(current, sizeof(current), "%s", resp.location);
            http_close(&http);
            continue;
        }

        if (resp.status_code != 206 || resp.content_range_start != start ||
            resp.content_range_end < start ||
            (expected_size > 0 && resp.content_range_total != expected_size)) {
            snprintf(err, err_n, "server did not return the expected byte range");
            http_close(&http);
            return -1;
        }

        int64_t body_length = resp.content_range_end - resp.content_range_start + 1;
        if (body_length <= 0 || body_length > SOURCE_SAMPLE_SIZE) {
            snprintf(err, err_n, "invalid sample length");
            http_close(&http);
            return -1;
        }

        char* buffer = (char*)malloc(SOURCE_SAMPLE_SIZE);
        if (!buffer) {
            snprintf(err, err_n, "out of memory");
            http_close(&http);
            return -1;
        }

        int total = 0;
        uint32_t value = 0;
        while (total < body_length) {
            int want = (int)(body_length - total);
            int got = http_read_body(&http, buffer, want);
            if (got <= 0) break;
            value = crc32_update(value, buffer, got);
            total += got;
        }
        free(buffer);
        http_close(&http);

        if (total != body_length) {
            snprintf(err, err_n, "sample ended early (%d/%lld bytes)",
                     total, (long long)body_length);
            return -1;
        }

        snprintf(url, url_n, "%s", current);
        snprintf(path_out, path_n, "%s", path);
        response_validator(&resp, validator, validator_n);
        *crc = value;
        *sample_length = total;
        *total_size = resp.content_range_total;
        return 0;
    }

    snprintf(err, err_n, "too many redirects");
    return -1;
}

int source_probe(download_source_t* source, const char* url,
                 int timeout_sec, const char* user_agent,
                 const char* referer, const char** extra_headers,
                 int extra_count, const proxy_config_t* proxy,
                 const char* interface_name,
                 char* err, int err_n) {
    memset(source, 0, sizeof(*source));
    snprintf(source->url, sizeof(source->url), "%s", url);

    int64_t total = 0;
    if (fetch_sample(source->url, sizeof(source->url), 0,
                     SOURCE_SAMPLE_SIZE - 1, 0, timeout_sec,
                     user_agent, referer, extra_headers, extra_count,
                     proxy, interface_name, &source->first_crc32,
                     &source->first_length, source->validator,
                     sizeof(source->validator), source->path,
                     sizeof(source->path), &total, err, err_n) != 0)
        return -1;
    source->file_size = total;

    int64_t last_start = source->file_size > SOURCE_SAMPLE_SIZE
                       ? source->file_size - SOURCE_SAMPLE_SIZE : 0;
    char last_validator[320];
    char last_path[HTTP_MAX_PATH];
    int64_t last_total = 0;
    if (fetch_sample(source->url, sizeof(source->url), last_start,
                     source->file_size - 1, source->file_size,
                     timeout_sec, user_agent, referer,
                     extra_headers, extra_count, proxy, interface_name,
                     &source->last_crc32, &source->last_length,
                     last_validator, sizeof(last_validator),
                     last_path, sizeof(last_path), &last_total,
                     err, err_n) != 0)
        return -1;
    if (last_total != source->file_size) {
        snprintf(err, err_n, "remote size changed during source check");
        return -1;
    }
    if (source->validator[0] &&
        (!last_validator[0] || strcmp(source->validator, last_validator) != 0)) {
        snprintf(err, err_n, "remote validator changed during source check");
        return -1;
    }
    snprintf(source->path, sizeof(source->path), "%s", last_path);
    return 0;
}

bool source_matches(const download_source_t* source,
                    const download_source_t* reference) {
    return source->file_size == reference->file_size &&
           source->first_length == reference->first_length &&
           source->last_length == reference->last_length &&
           source->first_crc32 == reference->first_crc32 &&
           source->last_crc32 == reference->last_crc32;
}
