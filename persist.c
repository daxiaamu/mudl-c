#include "persist.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int utf8_to_wide(const char* src, wchar_t* dst, int dst_n) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_n);
    if (n > 0) return 0;
    return MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_n) > 0 ? 0 : -1;
}

/* On-disk segment entry (40 bytes each) */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* PERSIST_MAGIC */
    uint32_t version;
    int64_t  file_size;
    uint32_t segment_count;
    uint32_t thread_count;  /* saved thread count for resume */
    uint32_t pad;           /* alignment */
    uint64_t timestamp;
    char     validator[256]; /* ETag or Last-Modified from the remote object */
} persist_header_t;

typedef struct {
    uint32_t index;
    int64_t  start_offset;
    int64_t  end_offset;
    int64_t  downloaded;
    uint32_t state;
    uint32_t retry_count;
    uint32_t crc32;
} persist_segment_t;
#pragma pack(pop)

#define SEG_ENTRY_SIZE (sizeof(persist_segment_t))

static void persist_output_path(const char* seg_path, char* out_path, int out_path_n) {
    snprintf(out_path, out_path_n, "%s", seg_path);
    const char* suffix = ".segments";
    size_t len = strlen(out_path);
    size_t suffix_len = strlen(suffix);
    if (len >= suffix_len && strcmp(out_path + len - suffix_len, suffix) == 0)
        out_path[len - suffix_len] = 0;
}

static int file_crc32_range(const char* path, int64_t offset, int64_t len, uint32_t* out_crc) {
    *out_crc = 0;
    if (len <= 0) return 0;

    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0)
        return -1;

    HANDLE hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return -1;

    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
        CloseHandle(hFile);
        return -1;
    }

    uint8_t buf[65536];
    uint32_t crc = 0;
    int64_t remaining = len;
    while (remaining > 0) {
        DWORD want = remaining > (int64_t)sizeof(buf) ? (DWORD)sizeof(buf) : (DWORD)remaining;
        DWORD got = 0;
        if (!ReadFile(hFile, buf, want, &got, NULL) || got == 0) {
            CloseHandle(hFile);
            return -1;
        }
        crc = crc32_update(crc, buf, (int)got);
        remaining -= got;
    }

    CloseHandle(hFile);
    *out_crc = crc;
    return 0;
}

void persist_path(const char* output_path, char* seg_path, int seg_path_n) {
    _snprintf(seg_path, seg_path_n, "%s.segments", output_path);
}

bool persist_exists(const char* segfile_path) {
    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(segfile_path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0)
        return false;
    DWORD attr = GetFileAttributesW(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int persist_save(const char* path, segment_manager_t* mgr, int64_t file_size,
                 int thread_count, const char* validator) {
    /* Save to .tmp first, then rename (atomic write) */
    char tmp_path[MAX_PATH * 2];
    _snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    wchar_t wpath[MAX_PATH * 2];
    wchar_t wtmp_path[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0 ||
        utf8_to_wide(tmp_path, wtmp_path, (int)(sizeof(wtmp_path) / sizeof(wtmp_path[0]))) != 0) {
        trace("persist: cannot convert path to UTF-16");
        return -1;
    }

    HANDLE hFile = CreateFileW(wtmp_path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        trace("persist: cannot create %s (error %lu)", tmp_path, GetLastError());
        return -1;
    }

    DWORD written;

    /* Write header */
    persist_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = PERSIST_MAGIC;
    hdr.version = PERSIST_VERSION;
    hdr.file_size = file_size;
    hdr.segment_count = mgr->segment_count;
    hdr.thread_count = thread_count;
    hdr.timestamp = GetTickCount64();
    if (validator)
        snprintf(hdr.validator, sizeof(hdr.validator), "%s", validator);

    if (!WriteFile(hFile, &hdr, sizeof(hdr), &written, NULL) || written != sizeof(hdr)) {
        CloseHandle(hFile); DeleteFileW(wtmp_path);
        trace("persist: header write failed"); return -1;
    }

    /* Write segments */
    EnterCriticalSection(&mgr->lock);
    for (int i = 0; i < mgr->segment_count; i++) {
        segment_t* s = &mgr->segments[i];
        persist_segment_t ps;
        memset(&ps, 0, sizeof(ps));
        ps.index = s->index;
        ps.start_offset = s->start_offset;
        ps.end_offset = s->end_offset;
        ps.downloaded = s->downloaded;
        ps.state = (uint32_t)s->state;
        ps.retry_count = s->retry_count;
        ps.crc32 = s->crc32;

        if (!WriteFile(hFile, &ps, sizeof(ps), &written, NULL) || written != sizeof(ps)) {
            LeaveCriticalSection(&mgr->lock);
            CloseHandle(hFile); DeleteFileW(wtmp_path);
            trace("persist: segment write failed"); return -1;
        }
    }
    LeaveCriticalSection(&mgr->lock);

    CloseHandle(hFile);

    /* Atomic rename */
    if (!MoveFileExW(wtmp_path, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        trace("persist: rename failed (error %lu)", GetLastError());
        DeleteFileW(wtmp_path);
        return -1;
    }

    trace("persist: saved %d segments to %s", mgr->segment_count, path);
    return 0;
}

int persist_load(const char* path, segment_manager_t* mgr, int64_t file_size,
                 int* thread_count, const char* validator) {
    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0)
        return -1;
    HANDLE hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    printf("Resume check: loading state\n");

    DWORD read;
    persist_header_t hdr;
    if (!ReadFile(hFile, &hdr, sizeof(hdr), &read, NULL) || read != sizeof(hdr)) {
        CloseHandle(hFile); return -1;
    }

    /* Validate header */
    if (hdr.magic != PERSIST_MAGIC || hdr.version != PERSIST_VERSION) {
        trace("persist: bad magic/version");
        CloseHandle(hFile); return -1;
    }

    /* If file size changed (different file), abort resume */
    if (hdr.file_size != file_size) {
        trace("persist: file size mismatch (saved=%lld, actual=%lld)",
              (long long)hdr.file_size, (long long)file_size);
        CloseHandle(hFile); return -1;
    }

    /* Size alone cannot distinguish a same-sized replacement. */
    if (!validator || !validator[0] || !hdr.validator[0] ||
        strcmp(hdr.validator, validator) != 0) {
        trace("persist: remote validator missing or changed");
        printf("Resume check: remote file identity changed or unavailable; restarting download\n");
        CloseHandle(hFile); return -1;
    }

    /* Restore saved thread count */
    if (thread_count) *thread_count = (int)hdr.thread_count;

    int seg_count = (int)hdr.segment_count;
    if (seg_count <= 0 || seg_count > 1024) {
        trace("persist: invalid segment count %d", seg_count);
        CloseHandle(hFile); return -1;
    }

    /* Initialize mgr with loaded segments */
    mgr->segment_count = 0;
    mgr->active_count = 0;
    mgr->complete_count = 0;
    mgr->file_size = file_size;
    mgr->pending_capacity = seg_count + 4;
    mgr->pending_queue = (int*)calloc(mgr->pending_capacity, sizeof(int));
    mgr->segments = (segment_t*)calloc(seg_count, sizeof(segment_t));
    if (!mgr->segments || !mgr->pending_queue) {
        CloseHandle(hFile); return -1;
    }
    mgr->pending_head = 0;
    mgr->pending_tail = -1;

    InitializeCriticalSection(&mgr->lock);
    InitializeConditionVariable(&mgr->cv_new_work);
    InitializeConditionVariable(&mgr->cv_done);

    /* Read segment entries */
    char out_path[MAX_PATH * 2];
    persist_output_path(path, out_path, sizeof(out_path));
    persist_segment_t ps;
    int pending_idx = 0;
    int64_t restored_total = 0;
    int64_t previous_end = -1;
    int64_t verified_total = 0;
    for (int i = 0; i < seg_count; i++) {
        if (!ReadFile(hFile, &ps, sizeof(ps), &read, NULL) || read != sizeof(ps)) {
            trace("persist: short read at segment %d", i);
            CloseHandle(hFile);
            segmgr_destroy(mgr);
            return -1;
        }

        int64_t seg_len = ps.end_offset - ps.start_offset + 1;
        if (ps.start_offset < 0 || ps.end_offset < ps.start_offset ||
            ps.end_offset >= file_size || ps.start_offset <= previous_end ||
            ps.downloaded < 0 || ps.downloaded > seg_len ||
            ps.state > SEG_SUSPENDED) {
            trace("persist: invalid segment %d", i);
            CloseHandle(hFile);
            segmgr_destroy(mgr);
            return -1;
        }
        if (i == 0 && ps.start_offset != 0) {
            trace("persist: first segment does not start at zero");
            CloseHandle(hFile);
            segmgr_destroy(mgr);
            return -1;
        }
        if (i > 0 && ps.start_offset != previous_end + 1) {
            trace("persist: segment gap/overlap at %d", i);
            CloseHandle(hFile);
            segmgr_destroy(mgr);
            return -1;
        }
        if (ps.state == SEG_COMPLETE && ps.downloaded != seg_len) {
            trace("persist: complete segment %d has partial byte count", i);
            CloseHandle(hFile);
            segmgr_destroy(mgr);
            return -1;
        }
        if (ps.downloaded > 0) {
            uint32_t actual_crc = 0;
            printf("Resume check: verifying segment %d/%d (%lld bytes)\n",
                   i + 1, seg_count, (long long)ps.downloaded);
            if (file_crc32_range(out_path, ps.start_offset, ps.downloaded, &actual_crc) != 0 ||
                actual_crc != ps.crc32) {
                printf("Resume check: segment %d failed, restarting download\n", i + 1);
                trace("persist: crc mismatch at segment %d", i);
                CloseHandle(hFile);
                segmgr_destroy(mgr);
                return -1;
            }
            verified_total += ps.downloaded;
            printf("Resume check: segment %d OK (%lld bytes verified)\n",
                   i + 1, (long long)verified_total);
        }
        previous_end = ps.end_offset;
        restored_total += ps.downloaded;
        if (restored_total > file_size) {
            trace("persist: restored total exceeds file size");
            CloseHandle(hFile);
            segmgr_destroy(mgr);
            return -1;
        }

        segment_t* s = &mgr->segments[mgr->segment_count++];
        s->index = i;
        s->start_offset = ps.start_offset;
        s->end_offset = ps.end_offset;
        s->downloaded = ps.downloaded;
        s->retry_count = ps.retry_count;
        s->crc32 = ps.crc32;
        /* Active/suspended work restarts as pending; completed ranges stay complete. */
        s->state = (ps.state == SEG_COMPLETE) ? SEG_COMPLETE : SEG_PENDING;
        s->socket_fd = -1;
        s->speed_bps = 0;

        /* Enqueue non-complete segments */
        if (ps.state != SEG_COMPLETE) {
            mgr->pending_queue[pending_idx++] = i;
        } else {
            mgr->complete_count++;
        }
    }
    mgr->pending_tail = pending_idx - 1;

    if (previous_end != file_size - 1) {
        trace("persist: segments do not cover full file");
        CloseHandle(hFile);
        segmgr_destroy(mgr);
        return -1;
    }

    CloseHandle(hFile);

    printf("Resume check: verified %lld bytes, resuming\n",
           (long long)verified_total);

    trace("persist: loaded %d segments (%d complete, %d pending) from %s",
          mgr->segment_count, mgr->complete_count,
          mgr->segment_count - mgr->complete_count, path);
    return 0;
}

void persist_remove(const char* path) {
    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) == 0)
        DeleteFileW(wpath);
    char bak[MAX_PATH * 2];
    _snprintf(bak, sizeof(bak), "%s.bak", path);
    wchar_t wbak[MAX_PATH * 2];
    if (utf8_to_wide(bak, wbak, (int)(sizeof(wbak) / sizeof(wbak[0]))) == 0)
        DeleteFileW(wbak);
    trace("persist: removed %s", path);
}
