#include "utils.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

void trace(const char* fmt, ...) {
    static bool debug = false;
    static bool checked = false;
    if (!checked) {
        debug = (GetEnvironmentVariableA("MUDM_DEBUG", NULL, 0) > 0);
        checked = true;
    }
    if (!debug) return;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[TRACE] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

uint64_t now_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return (ui.QuadPart / 10000ULL) - 11644473600000ULL;
}

void sleep_ms(uint32_t ms) {
    Sleep(ms);
}

char* str_trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == 0) return s;
    char* end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end--;
    end[1] = 0;
    return s;
}

char* str_join_path(const char* dir, const char* file) {
    static char buf[MAX_PATH * 2];
    _snprintf(buf, sizeof(buf), "%s/%s", dir, file);
    for (int i = 0; buf[i]; i++)
        if (buf[i] == '/') buf[i] = '\\';
    return buf;
}

bool str_ends_with(const char* s, const char* suffix) {
    size_t sl = strlen(s);
    size_t sufl = strlen(suffix);
    if (sufl > sl) return false;
    return strcmp(s + sl - sufl, suffix) == 0;
}

const char* fmt_bytes(int64_t bytes) {
    static char buf[32];
    if (bytes < 1024)
        _snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    else if (bytes < 1024LL * 1024)
        _snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        _snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else
        _snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

const char* fmt_speed(int64_t bps) {
    static char buf[32];
    if (bps < 1024)
        _snprintf(buf, sizeof(buf), "%lld B/s", (long long)bps);
    else if (bps < 1024LL * 1024)
        _snprintf(buf, sizeof(buf), "%.1f KB/s", bps / 1024.0);
    else
        _snprintf(buf, sizeof(buf), "%.1f MB/s", bps / (1024.0 * 1024));
    return buf;
}

const char* fmt_pct(double pct) {
    static char buf[16];
    _snprintf(buf, sizeof(buf), "%.1f%%", pct);
    return buf;
}

void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "WARNING: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

bool console_has_color(void) {
    static int cached = -1;
    if (cached == -1) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE) { cached = 0; return false; }
        DWORD mode;
        if (!GetConsoleMode(h, &mode)) { cached = 0; return false; }
        cached = 1;
    }
    return cached != 0;
}
