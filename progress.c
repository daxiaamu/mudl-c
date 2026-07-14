#include "progress.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

void progress_init(progress_t* p, progress_mode_t mode,
                   int64_t total, int64_t initial_downloaded, int threads) {
    memset(p, 0, sizeof(progress_t));
    p->mode = mode;
    p->total = total;
    p->downloaded = initial_downloaded;
    p->initial_downloaded = initial_downloaded;
    p->threads_total = threads;
    p->start_time_ms = now_ms();
}

void progress_update(progress_t* p, int64_t downloaded,
                     int64_t speed, int active, int total) {
    uint64_t now = now_ms();
    p->downloaded = downloaded;
    p->speed_bps = speed;
    p->threads_active = active;
    p->threads_total = total;

    if (p->mode == PROGRESS_SILENT) return;

    if (p->mode == PROGRESS_JSON) {
        double pct = p->total > 0 ? (100.0 * downloaded / p->total) : 0.0;
        int eta = (speed > 0 && p->total > 0)
                  ? (int)((p->total - downloaded) / speed)
                  : 0;
        printf("{\"t\":%llu,\"dl\":%lld,\"sz\":%lld,\"sp\":%lld,"
               "\"p\":%.1f,\"th\":%d,\"e\":%d,\"s\":\"dl\"}\n",
               (unsigned long long)(p->start_time_ms / 1000),
               (long long)downloaded, (long long)p->total,
               (long long)speed, pct, active, eta);
        fflush(stdout);
        return;
    }

    if (p->mode == PROGRESS_LINE) {
        bool done = p->total > 0 && downloaded >= p->total;
        if (!done && p->last_line_ms > 0 && now - p->last_line_ms < 1000)
            return;

        double pct = p->total > 0 ? (100.0 * downloaded / p->total) : 0.0;
        int eta_sec = (speed > 0 && p->total > 0)
                      ? (int)((p->total - downloaded) / speed)
                      : 0;
        char downloaded_s[32], total_s[32], speed_s[32], eta_s[32];
        snprintf(downloaded_s, sizeof(downloaded_s), "%s", fmt_bytes(downloaded));
        snprintf(total_s, sizeof(total_s), "%s", fmt_bytes(p->total));
        snprintf(speed_s, sizeof(speed_s), "%s", fmt_speed(speed));
        if (eta_sec > 0) {
            int h = eta_sec / 3600;
            int m = (eta_sec % 3600) / 60;
            int s = eta_sec % 60;
            if (h > 0)
                snprintf(eta_s, sizeof(eta_s), "%d:%02d:%02d", h, m, s);
            else
                snprintf(eta_s, sizeof(eta_s), "%d:%02d", m, s);
        } else {
            snprintf(eta_s, sizeof(eta_s), "--");
        }

        printf("%.1f%% %s/%s %s ETA %s T:%d/%d\n",
               pct, downloaded_s, total_s, speed_s, eta_s, active, total);
        fflush(stdout);
        p->last_line_ms = now;
        return;
    }

    /* BAR mode */
    if (!console_has_color()) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

    int width = csbi.dwSize.X;
    if (width < 40) width = 40;
    if (width > 120) width = 120;

    char buf[256];
    int pos = 0;

    double pct = p->total > 0 ? (100.0 * downloaded / p->total) : 0.0;
    int eta_sec = (speed > 0 && p->total > 0)
                  ? (int)((p->total - downloaded) / speed)
                  : 0;

    int bar_w = width - 55;
    if (bar_w < 10) bar_w = 10;
    if (bar_w > 50) bar_w = 50;

    int filled = (int)(bar_w * downloaded / (p->total > 0 ? p->total : 1));
    if (filled > bar_w) filled = bar_w;

    pos += _snprintf(buf + pos, sizeof(buf) - pos, "\r");
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "%5.1f%% ", pct);
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "[");
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) buf[pos++] = '=';
        else if (i == filled) buf[pos++] = '>';
        else buf[pos++] = ' ';
    }
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "] ");
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "%s/",
                     fmt_bytes(downloaded));
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "%s ",
                     fmt_bytes(p->total));
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "%s ",
                     fmt_speed(speed));
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "T:%d/%d ", active, total);

    if (eta_sec > 0) {
        int h = eta_sec / 3600;
        int m = (eta_sec % 3600) / 60;
        int s = eta_sec % 60;
        if (h > 0)
            pos += _snprintf(buf + pos, sizeof(buf) - pos, "%d:%02d:%02d", h, m, s);
        else
            pos += _snprintf(buf + pos, sizeof(buf) - pos, "%d:%02d", m, s);
    }

    pos += _snprintf(buf + pos, sizeof(buf) - pos, "\x1b[K");

    printf("%s", buf);
    fflush(stdout);
}

void progress_done(progress_t* p) {
    p->finished = true;
    uint64_t elapsed_ms = now_ms() - p->start_time_ms;
    double elapsed_sec = elapsed_ms / 1000.0;
    int64_t session_downloaded = p->downloaded - p->initial_downloaded;
    if (session_downloaded < 0) session_downloaded = 0;

    if (p->mode == PROGRESS_BAR || p->mode == PROGRESS_LINE) {
        printf("\n");
        printf("Downloaded %s (avg %s)\n",
               fmt_bytes(p->downloaded),
               fmt_speed(elapsed_sec > 0
                         ? (int64_t)(session_downloaded / elapsed_sec)
                         : 0));
    } else if (p->mode == PROGRESS_JSON) {
        printf("{\"t\":%llu,\"dl\":%lld,\"sz\":%lld,\"elapsed\":%.1f,"
               "\"s\":\"done\"}\n",
               (unsigned long long)(p->start_time_ms / 1000),
               (long long)p->downloaded, (long long)p->total, elapsed_sec);
        fflush(stdout);
    }
}

void progress_log(progress_t* p, const char* msg) {
    if (p->mode == PROGRESS_BAR && console_has_color()) {
        printf("\r\x1b[K%s\n", msg);
        fflush(stdout);
    } else if (p->mode != PROGRESS_SILENT) {
        printf("%s\n", msg);
        fflush(stdout);
    }
}
