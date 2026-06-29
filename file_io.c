#include "file_io.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

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
                CreateDirectoryA(tmp, NULL);
                *p = '\\';
            }
        }
        CreateDirectoryA(tmp, NULL);
    }

    f->hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f->hFile == INVALID_HANDLE_VALUE) {
        _snprintf(f->last_error, sizeof(f->last_error),
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
        _snprintf(f->last_error, sizeof(f->last_error),
                  "SetFilePointerEx error: %lu", GetLastError());
        LeaveCriticalSection(&f->lock);
        return -1;
    }
    DWORD written;
    if (!WriteFile(f->hFile, data, len, &written, NULL)) {
        _snprintf(f->last_error, sizeof(f->last_error),
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
        _snprintf(f->last_error, sizeof(f->last_error),
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
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t file_size(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
        return -1;
    LARGE_INTEGER li;
    li.LowPart = info.nFileSizeLow;
    li.HighPart = info.nFileSizeHigh;
    return li.QuadPart;
}

int file_name_from_url(const char* url, char* name, int name_n,
                       const char* content_disposition) {
    (void)content_disposition;

    const char* last_slash = strrchr(url, '/');
    if (last_slash && last_slash[1]) {
        const char* fn = last_slash + 1;
        const char* q = strchr(fn, '?');
        size_t len = q ? (size_t)(q - fn) : strlen(fn);
        if (len > 0 && len < (size_t)name_n) {
            char* dst = name;
            for (size_t i = 0; i < len && dst - name < name_n - 1; i++) {
                if (fn[i] == '%' && i + 2 < len) {
                    char hex[3] = {fn[i+1], fn[i+2], 0};
                    *dst++ = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    *dst++ = fn[i];
                }
            }
            *dst = 0;
            return 0;
        }
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
    _snprintf(raw, sizeof(raw), "%s\\%s", dir ? dir : ".", name);

    if (strlen(raw) >= MAX_PATH - 12) {
        _snprintf(out, out_n, "\\\\?\\%s", raw);
    } else {
        strncpy(out, raw, out_n - 1);
    }

    for (char* p = out; *p; p++)
        if (*p == '/') *p = '\\';

    return 0;
}
