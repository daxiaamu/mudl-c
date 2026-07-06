#include "file_io.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int utf8_to_wide(const char* src, wchar_t* dst, int dst_n) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_n);
    if (n > 0) return 0;
    return MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_n) > 0 ? 0 : -1;
}

static void url_decode_part(const char* src, size_t len, char* out, int out_n,
                            bool plus_to_space) {
    char* dst = out;
    char* end = out + out_n - 1;
    for (size_t i = 0; i < len && dst < end; i++) {
        if (src[i] == '%' && i + 2 < len) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            char* hex_end = NULL;
            long v = strtol(hex, &hex_end, 16);
            if (hex_end && *hex_end == 0) {
                *dst++ = (char)v;
                i += 2;
                continue;
            }
        }
        if (plus_to_space && src[i] == '+')
            *dst++ = ' ';
        else
            *dst++ = src[i];
    }
    *dst = 0;
}

static bool basename_from_value(const char* value, char* name, int name_n) {
    if (!value || !value[0]) return false;

    const char* last_slash = strrchr(value, '/');
    const char* last_backslash = strrchr(value, '\\');
    const char* base = value;
    if (last_slash && last_slash + 1 > base) base = last_slash + 1;
    if (last_backslash && last_backslash + 1 > base) base = last_backslash + 1;

    if (!base[0]) return false;
    snprintf(name, name_n, "%s", base);
    name[name_n - 1] = 0;
    return true;
}

static bool is_generic_url_name(const char* name) {
    if (!name || !name[0]) return true;
    return _stricmp(name, "download") == 0 ||
           _stricmp(name, "download.php") == 0 ||
           _stricmp(name, "downloadCheck") == 0 ||
           _stricmp(name, "downloadCheck.php") == 0 ||
           _stricmp(name, "index.php") == 0;
}

static bool filename_from_query(const char* query, char* name, int name_n) {
    static const char* keys[] = {
        "file=", "filename=", "fileName=", "name=", "fin=", NULL
    };

    if (!query) return false;
    const char* p = query;
    while (*p) {
        const char* next = strchr(p, '&');
        size_t pair_len = next ? (size_t)(next - p) : strlen(p);

        for (int i = 0; keys[i]; i++) {
            size_t key_len = strlen(keys[i]);
            if (pair_len > key_len && _strnicmp(p, keys[i], key_len) == 0) {
                char decoded[MAX_PATH * 2];
                url_decode_part(p + key_len, pair_len - key_len,
                                decoded, sizeof(decoded), true);
                if (basename_from_value(decoded, name, name_n))
                    return true;
            }
        }

        if (!next) break;
        p = next + 1;
    }

    return false;
}

int file_open(file_t* f, const char* path, int64_t expected_size) {
    memset(f, 0, sizeof(file_t));
    strncpy(f->path, path, MAX_PATH - 1);

    /* Create parent directory */
    char dir[MAX_PATH];
    strncpy(dir, path, MAX_PATH - 1);
    char* sep = strrchr(dir, '\\');
    if (sep) {
        *sep = 0;
        char tmp[MAX_PATH];
        strncpy(tmp, dir, MAX_PATH - 1);
        for (char* p = tmp + 3; *p; p++) {
            if (*p == '\\') {
                *p = 0;
                wchar_t wtmp[MAX_PATH * 2];
                if (utf8_to_wide(tmp, wtmp, (int)(sizeof(wtmp) / sizeof(wtmp[0]))) == 0)
                    CreateDirectoryW(wtmp, NULL);
                *p = '\\';
            }
        }
        wchar_t wtmp[MAX_PATH * 2];
        if (utf8_to_wide(tmp, wtmp, (int)(sizeof(wtmp) / sizeof(wtmp[0]))) == 0)
            CreateDirectoryW(wtmp, NULL);
    }

    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0) {
        snprintf(f->last_error, sizeof(f->last_error),
                 "Cannot convert path to UTF-16: %s", path);
        return -1;
    }

    f->hFile = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f->hFile == INVALID_HANDLE_VALUE) {
        snprintf(f->last_error, sizeof(f->last_error),
                 "Cannot create file: %s (error %lu)", path, GetLastError());
        return -1;
    }

    /* Pre-allocate space */
    if (expected_size > 0) {
        LARGE_INTEGER li;
        li.QuadPart = expected_size;
        SetFilePointerEx(f->hFile, li, NULL, FILE_BEGIN);
        SetEndOfFile(f->hFile);
        li.QuadPart = 0;
        SetFilePointerEx(f->hFile, li, NULL, FILE_BEGIN);
    }

    f->size = expected_size;
    InitializeCriticalSection(&f->lock);
    f->opened = true;
    return 0;
}

int file_write_at(file_t* f, int64_t offset, const char* data, int len) {
    EnterCriticalSection(&f->lock);
    LARGE_INTEGER li;
    li.QuadPart = offset;
    BOOL ok = SetFilePointerEx(f->hFile, li, NULL, FILE_BEGIN);
    if (!ok) {
        snprintf(f->last_error, sizeof(f->last_error),
                 "SetFilePointerEx error: %lu", GetLastError());
        LeaveCriticalSection(&f->lock);
        return -1;
    }
    DWORD written;
    if (!WriteFile(f->hFile, data, len, &written, NULL)) {
        snprintf(f->last_error, sizeof(f->last_error),
                 "WriteFile error: %lu at offset %lld",
                 GetLastError(), (long long)offset);
        LeaveCriticalSection(&f->lock);
        return -1;
    }
    LeaveCriticalSection(&f->lock);
    return (int)written;
}

int file_write(file_t* f, const char* data, int len) {
    DWORD written;
    if (!WriteFile(f->hFile, data, len, &written, NULL)) {
        snprintf(f->last_error, sizeof(f->last_error),
                 "WriteFile error: %lu", GetLastError());
        return -1;
    }
    return (int)written;
}

void file_close(file_t* f) {
    if (f->hFile && f->hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(f->hFile);
    }
    DeleteCriticalSection(&f->lock);
    f->hFile = NULL;
    f->opened = false;
}

bool file_exists(const char* path) {
    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0)
        return false;
    DWORD attr = GetFileAttributesW(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t file_size(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0)
        return -1;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &info))
        return -1;
    LARGE_INTEGER li;
    li.LowPart = info.nFileSizeLow;
    li.HighPart = info.nFileSizeHigh;
    return li.QuadPart;
}

int file_name_from_url(const char* url, char* name, int name_n,
                       const char* content_disposition) {
    (void)content_disposition;

    char query_name[256] = {0};
    const char* query = strchr(url, '?');
    if (query)
        filename_from_query(query + 1, query_name, sizeof(query_name));

    const char* last_slash = strrchr(url, '/');
    if (last_slash && last_slash[1]) {
        const char* fn = last_slash + 1;
        const char* q = strchr(fn, '?');
        size_t len = q ? (size_t)(q - fn) : strlen(fn);
        if (len > 0 && len < (size_t)name_n) {
            url_decode_part(fn, len, name, name_n, false);
            if (query_name[0] && is_generic_url_name(name))
                snprintf(name, name_n, "%s", query_name);
            name[name_n - 1] = 0;
            return 0;
        }
    }

    if (query_name[0]) {
        snprintf(name, name_n, "%s", query_name);
        name[name_n - 1] = 0;
        return 0;
    }

    const char* host_start = strstr(url, "://");
    if (host_start) {
        host_start += 3;
        const char* host_end = strchr(host_start, '/');
        size_t len = host_end ? (size_t)(host_end - host_start) : strlen(host_start);
        if (len > 0 && len < (size_t)name_n - 5) {
            memcpy(name, host_start, len);
            name[len] = 0;
            strcat(name, ".download");
            return 0;
        }
    }

    strncpy(name, "download", name_n - 1);
    return 0;
}

int file_make_safe_path(const char* dir, const char* name,
                        char* out, int out_n) {
    char raw[MAX_PATH * 2];
    const char* base = dir ? dir : ".";
    size_t base_len = strlen(base);
    if (base_len > 0 && (base[base_len - 1] == '\\' || base[base_len - 1] == '/'))
        snprintf(raw, sizeof(raw), "%s%s", base, name);
    else
        snprintf(raw, sizeof(raw), "%s\\%s", base, name);

    if (strlen(raw) >= MAX_PATH - 12) {
        snprintf(out, out_n, "\\\\?\\%s", raw);
    } else {
        strncpy(out, raw, out_n - 1);
    }

    for (char* p = out; *p; p++)
        if (*p == '/') *p = '\\';

    return 0;
}

static bool path_is_absolute(const char* path) {
    if (!path || !path[0]) return false;
    if ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'))
        return true;
    return ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' &&
           (path[2] == '\\' || path[2] == '/');
}

static bool path_has_separator(const char* path) {
    return path && (strchr(path, '\\') || strchr(path, '/'));
}

static void normalize_output_path(const char* raw, char* out, int out_n) {
    snprintf(out, out_n, "%s", raw ? raw : "");
    out[out_n - 1] = 0;

    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '"') {
        out[--len] = 0;
    }

    for (char* p = out; *p; p++)
        if (*p == '/') *p = '\\';

    if (strlen(out) >= MAX_PATH - 12 &&
        strncmp(out, "\\\\?\\", 4) != 0 &&
        path_is_absolute(out)) {
        char tmp[MAX_PATH * 2];
        snprintf(tmp, sizeof(tmp), "\\\\?\\%s", out);
        snprintf(out, out_n, "%s", tmp);
    }
}

int file_resolve_output_path(const char* url, const char* dir,
                             const char* output, char* out, int out_n) {
    char filename[256];
    file_name_from_url(url, filename, sizeof(filename), NULL);
    if (filename[0] == 0)
        strncpy(filename, "download", sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = 0;

    if (!output || output[0] == 0) {
        return file_make_safe_path(dir && dir[0] ? dir : ".", filename, out, out_n);
    }

    char cleaned[MAX_PATH * 2];
    normalize_output_path(output, cleaned, sizeof(cleaned));

    if (path_is_absolute(cleaned) || path_has_separator(cleaned)) {
        return -1;
    }

    return file_make_safe_path(dir && dir[0] ? dir : ".", cleaned, out, out_n);
}
