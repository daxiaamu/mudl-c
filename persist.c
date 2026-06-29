#include "persist.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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
} persist_header_t;

typedef struct {
    uint32_t index;
    int64_t  start_offset;
    int64_t  end_offset;
    int64_t  downloaded;
    uint32_t state;
    uint32_t retry_count;
    uint8_t  reserved[4];
} persist_segment_t;
#pragma pack(pop)

#define SEG_ENTRY_SIZE (sizeof(persist_segment_t))

void persist_path(const char* output_path, char* seg_path, int seg_path_n) {
    _snprintf(seg_path, seg_path_n, "%s.segments", output_path);
}

bool persist_exists(const char* segfile_path) {
    DWORD attr = GetFileAttributesA(segfile_path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int persist_save(const char* path, segment_manager_t* mgr, int64_t file_size, int thread_count) {
    /* Save to .tmp first, then rename (atomic write) */
    char tmp_path[MAX_PATH * 2];
    _snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    HANDLE hFile = CreateFileA(tmp_path, GENERIC_WRITE, 0, NULL,
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

    if (!WriteFile(hFile, &hdr, sizeof(hdr), &written, NULL) || written != sizeof(hdr)) {
        CloseHandle(hFile); DeleteFileA(tmp_path);
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

        if (!WriteFile(hFile, &ps, sizeof(ps), &written, NULL) || written != sizeof(ps)) {
            LeaveCriticalSection(&mgr->lock);
            CloseHandle(hFile); DeleteFileA(tmp_path);
            trace("persist: segment write failed"); return -1;
        }
    }
    LeaveCriticalSection(&mgr->lock);

    CloseHandle(hFile);

    /* Atomic rename */
    if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        trace("persist: rename failed (error %lu)", GetLastError());
        DeleteFileA(tmp_path);
        return -1;
    }

    trace("persist: saved %d segments to %s", mgr->segment_count, path);
    return 0;
}

int persist_load(const char* path, segment_manager_t* mgr, int64_t file_size, int* thread_count) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

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
    persist_segment_t ps;
    int pending_idx = 0;
    for (int i = 0; i < seg_count; i++) {
        if (!ReadFile(hFile, &ps, sizeof(ps), &read, NULL) || read != sizeof(ps)) {
            trace("persist: short read at segment %d", i);
            break;
        }

        segment_t* s = &mgr->segments[mgr->segment_count++];
        s->index = ps.index;
        s->start_offset = ps.start_offset;
        s->end_offset = ps.end_offset;
        s->downloaded = ps.downloaded;
        s->retry_count = ps.retry_count;
        s->state = SEG_PENDING;  /* Always restart as PENDING */
        s->socket_fd = -1;
        s->speed_bps = 0;

        /* Enqueue non-complete segments */
        if (ps.state != SEG_COMPLETE) {
            mgr->pending_queue[pending_idx++] = s->index;
        } else {
            mgr->complete_count++;
        }
    }
    mgr->pending_tail = pending_idx - 1;

    CloseHandle(hFile);

    trace("persist: loaded %d segments (%d complete, %d pending) from %s",
          mgr->segment_count, mgr->complete_count,
          mgr->segment_count - mgr->complete_count, path);
    return 0;
}

void persist_remove(const char* path) {
    DeleteFileA(path);
    char bak[MAX_PATH * 2];
    _snprintf(bak, sizeof(bak), "%s.bak", path);
    DeleteFileA(bak);
    trace("persist: removed %s", path);
}
