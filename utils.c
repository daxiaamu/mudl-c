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
    snprintf(buf, sizeof(buf), "%s/%s", dir, file);
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
        snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    else if (bytes < 1024LL * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

const char* fmt_speed(int64_t bps) {
    static char buf[32];
    if (bps < 1024)
        snprintf(buf, sizeof(buf), "%lld B/s", (long long)bps);
    else if (bps < 1024LL * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB/s", bps / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%.1f MB/s", bps / (1024.0 * 1024));
    return buf;
}

const char* fmt_pct(double pct) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%.1f%%", pct);
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

uint32_t crc32_update(uint32_t crc, const void* data, int len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }

    const unsigned char* p = (const unsigned char*)data;
    crc = ~crc;
    for (int i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t gf2_matrix_times(const uint32_t* matrix, uint32_t vector) {
    uint32_t sum = 0;
    while (vector) {
        if (vector & 1U) sum ^= *matrix;
        vector >>= 1;
        matrix++;
    }
    return sum;
}

static void gf2_matrix_square(uint32_t* square, const uint32_t* matrix) {
    for (int i = 0; i < 32; i++)
        square[i] = gf2_matrix_times(matrix, matrix[i]);
}

uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, int64_t len2) {
    if (len2 <= 0) return crc1;

    uint32_t even[32];
    uint32_t odd[32];
    odd[0] = 0xEDB88320U;
    uint32_t row = 1;
    for (int i = 1; i < 32; i++) {
        odd[i] = row;
        row <<= 1;
    }

    gf2_matrix_square(even, odd);
    gf2_matrix_square(odd, even);
    do {
        gf2_matrix_square(even, odd);
        if (len2 & 1) crc1 = gf2_matrix_times(even, crc1);
        len2 >>= 1;
        if (len2 == 0) break;

        gf2_matrix_square(odd, even);
        if (len2 & 1) crc1 = gf2_matrix_times(odd, crc1);
        len2 >>= 1;
    } while (len2 != 0);

    return crc1 ^ crc2;
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
