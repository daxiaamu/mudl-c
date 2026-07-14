#include "segment.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Caller must hold mgr->lock. Reuse consumed queue space before growing. */
static int pending_enqueue_locked(segment_manager_t* mgr, int index) {
    if (mgr->pending_tail + 1 >= mgr->pending_capacity) {
        int pending = mgr->pending_tail - mgr->pending_head + 1;
        if (pending > 0) {
            memmove(mgr->pending_queue,
                    mgr->pending_queue + mgr->pending_head,
                    (size_t)pending * sizeof(int));
            mgr->pending_head = 0;
            mgr->pending_tail = pending - 1;
        } else {
            mgr->pending_head = 0;
            mgr->pending_tail = -1;
        }
    }

    if (mgr->pending_tail + 1 >= mgr->pending_capacity) {
        int new_capacity = mgr->pending_capacity > 0
                         ? mgr->pending_capacity * 2 : 8;
        int* grown = (int*)realloc(mgr->pending_queue,
                                  (size_t)new_capacity * sizeof(int));
        if (!grown) return -1;
        mgr->pending_queue = grown;
        mgr->pending_capacity = new_capacity;
    }

    mgr->pending_queue[++mgr->pending_tail] = index;
    return 0;
}

int segmgr_init(segment_manager_t* mgr, int64_t file_size, int max_connections) {
    memset(mgr, 0, sizeof(segment_manager_t));
    mgr->file_size = file_size;
    mgr->max_retries = 5;
    mgr->worker_limit = max_connections;

    /* Determine segment count and size */
    int seg_count;
    if (file_size < MIN_SEGMENT_SIZE) {
        seg_count = 1;
    } else {
        int64_t seg_size = file_size / max_connections;
        if (seg_size < MIN_SEGMENT_SIZE) seg_size = MIN_SEGMENT_SIZE;
        seg_count = (int)(file_size / seg_size);
        if (seg_count < 1) seg_count = 1;
        if (seg_count > max_connections) seg_count = max_connections;
    }

    mgr->segment_count = seg_count;
    mgr->pending_capacity = seg_count + 4; /* extra space for rolled-back segments */
    mgr->pending_queue = (int*)calloc(mgr->pending_capacity, sizeof(int));
    if (!mgr->pending_queue) return -1;

    mgr->segments = (segment_t*)calloc(seg_count, sizeof(segment_t));
    if (!mgr->segments) { free(mgr->pending_queue); return -1; }

    /* Initial segmentation: equal chunks */
    int64_t seg_size = (file_size + seg_count - 1) / seg_count;
    int64_t offset = 0;
    for (int i = 0; i < seg_count; i++) {
        segment_t* s = &mgr->segments[i];
        s->index = i;
        s->start_offset = offset;
        s->end_offset = offset + seg_size - 1;
        if (s->end_offset >= file_size) s->end_offset = file_size - 1;
        s->downloaded = 0;
        s->state = SEG_PENDING;
        s->retry_count = 0;
        s->socket_fd = -1;
        s->speed_bps = 0;
        s->last_activity_ms = 0;
        mgr->pending_queue[i] = i;
        offset += seg_size;
        mgr->total_size += (int)(s->end_offset - s->start_offset + 1);
    }
    mgr->pending_tail = seg_count - 1;

    InitializeCriticalSection(&mgr->lock);
    InitializeConditionVariable(&mgr->cv_new_work);
    InitializeConditionVariable(&mgr->cv_done);

    return 0;
}

void segmgr_destroy(segment_manager_t* mgr) {
    DeleteCriticalSection(&mgr->lock);
    free(mgr->segments);
    free(mgr->pending_queue);
    memset(mgr, 0, sizeof(segment_manager_t));
}

segment_t* segmgr_acquire_pending(segment_manager_t* mgr) {
    EnterCriticalSection(&mgr->lock);

    if (mgr->worker_limit > 0 && mgr->active_count >= mgr->worker_limit) {
        LeaveCriticalSection(&mgr->lock);
        return NULL;
    }

    while (mgr->pending_head <= mgr->pending_tail) {
        int idx = mgr->pending_queue[mgr->pending_head++];
        segment_t* seg = &mgr->segments[idx];
        if (seg->state == SEG_PENDING) {
            seg->state = SEG_DOWNLOADING;
            seg->last_activity_ms = GetTickCount64();
            mgr->active_count++;
            LeaveCriticalSection(&mgr->lock);
            return seg;
        }
    }

    LeaveCriticalSection(&mgr->lock);
    return NULL;
}

void segmgr_set_worker_limit(segment_manager_t* mgr, int worker_limit) {
    if (worker_limit < 1) worker_limit = 1;
    EnterCriticalSection(&mgr->lock);
    mgr->worker_limit = worker_limit;
    LeaveCriticalSection(&mgr->lock);
    WakeAllConditionVariable(&mgr->cv_new_work);
}

int segmgr_compact_verified(segment_manager_t* mgr) {
    EnterCriticalSection(&mgr->lock);

    int old_count = mgr->segment_count;
    int write = 0;
    for (int read = 0; read < old_count; read++) {
        segment_t current = mgr->segments[read];
        if (write > 0 && mgr->segments[write - 1].state == SEG_COMPLETE) {
            segment_t* previous = &mgr->segments[write - 1];
            previous->crc32 = crc32_combine(previous->crc32, current.crc32,
                                            current.downloaded);
            previous->end_offset = current.end_offset;
            previous->downloaded += current.downloaded;
            if (current.state != SEG_COMPLETE) {
                previous->state = SEG_PENDING;
                previous->retry_count = current.retry_count;
            }
            previous->socket_fd = -1;
            previous->speed_bps = 0;
            previous->last_activity_ms = 0;
        } else {
            mgr->segments[write++] = current;
        }
    }

    mgr->segment_count = write;
    mgr->active_count = 0;
    mgr->complete_count = 0;
    mgr->pending_head = 0;
    mgr->pending_tail = -1;
    for (int i = 0; i < write; i++) {
        segment_t* segment = &mgr->segments[i];
        segment->index = i;
        if (segment->state == SEG_COMPLETE)
            mgr->complete_count++;
        else
            mgr->pending_queue[++mgr->pending_tail] = i;
    }

    LeaveCriticalSection(&mgr->lock);
    return old_count - write;
}

int segmgr_expand_pending(segment_manager_t* mgr, int target_count) {
    EnterCriticalSection(&mgr->lock);

    int pending_count = 0;
    for (int i = 0; i < mgr->segment_count; i++)
        if (mgr->segments[i].state == SEG_PENDING)
            pending_count++;

    while (pending_count < target_count) {
        int best_idx = -1;
        int64_t max_remaining = 0;
        for (int i = 0; i < mgr->segment_count; i++) {
            segment_t* s = &mgr->segments[i];
            if (s->state != SEG_PENDING) continue;
            int64_t remaining = s->end_offset - s->start_offset + 1 - s->downloaded;
            if (remaining >= 2LL * MIN_SEGMENT_SIZE && remaining > max_remaining) {
                max_remaining = remaining;
                best_idx = i;
            }
        }
        if (best_idx < 0) break;

        int old_count = mgr->segment_count;
        segment_t* grown = (segment_t*)realloc(
            mgr->segments, (size_t)(old_count + 1) * sizeof(segment_t));
        if (!grown) {
            LeaveCriticalSection(&mgr->lock);
            return -1;
        }
        mgr->segments = grown;

        memmove(&mgr->segments[best_idx + 2],
                &mgr->segments[best_idx + 1],
                (size_t)(old_count - best_idx - 1) * sizeof(segment_t));

        segment_t* original = &mgr->segments[best_idx];
        int64_t old_end = original->end_offset;
        int64_t new_len = max_remaining / 2;
        int64_t new_start = old_end - new_len + 1;
        original->end_offset = new_start - 1;

        segment_t* added = &mgr->segments[best_idx + 1];
        memset(added, 0, sizeof(*added));
        added->start_offset = new_start;
        added->end_offset = old_end;
        added->state = SEG_PENDING;
        added->socket_fd = -1;

        mgr->segment_count++;
        pending_count++;
        for (int i = 0; i < mgr->segment_count; i++)
            mgr->segments[i].index = i;
    }

    int needed = mgr->segment_count + 4;
    if (mgr->pending_capacity < needed) {
        int* grown = (int*)realloc(mgr->pending_queue,
                                  (size_t)needed * sizeof(int));
        if (!grown) {
            LeaveCriticalSection(&mgr->lock);
            return -1;
        }
        mgr->pending_queue = grown;
        mgr->pending_capacity = needed;
    }
    mgr->pending_head = 0;
    mgr->pending_tail = -1;
    for (int i = 0; i < mgr->segment_count; i++)
        if (mgr->segments[i].state == SEG_PENDING)
            mgr->pending_queue[++mgr->pending_tail] = i;

    LeaveCriticalSection(&mgr->lock);
    return pending_count;
}

void segmgr_complete(segment_manager_t* mgr, segment_t* seg) {
    EnterCriticalSection(&mgr->lock);
    seg->state = SEG_COMPLETE;
    seg->speed_bps = 0;
    mgr->active_count--;
    mgr->complete_count++;
    LeaveCriticalSection(&mgr->lock);
    WakeConditionVariable(&mgr->cv_done);
    WakeAllConditionVariable(&mgr->cv_new_work);
}

void segmgr_error(segment_manager_t* mgr, segment_t* seg) {
    EnterCriticalSection(&mgr->lock);
    seg->retry_count++;
    if (seg->retry_count <= mgr->max_retries) {
        /* retry_count is the number of retries already scheduled */
        int shift = seg->retry_count > 5 ? 5 : seg->retry_count;
        uint64_t delay = 1000ULL << shift;
        if (delay > 30000) delay = 30000;
        seg->state = SEG_SUSPENDED;
        seg->suspend_until_ms = GetTickCount64() + delay;
        mgr->active_count--;
        trace("Segment %d suspended for %llu ms (retry %d)",
              seg->index, (unsigned long long)delay, seg->retry_count);
    } else {
        seg->state = SEG_ERROR;
        mgr->active_count--;
        trace("Segment %d error (retries exhausted)", seg->index);
    }
    LeaveCriticalSection(&mgr->lock);
    WakeAllConditionVariable(&mgr->cv_new_work);
}

bool segmgr_all_done(segment_manager_t* mgr) {
    EnterCriticalSection(&mgr->lock);
    bool done = (mgr->complete_count == mgr->segment_count);
    LeaveCriticalSection(&mgr->lock);
    return done;
}

int64_t segmgr_total_downloaded(segment_manager_t* mgr) {
    int64_t total = 0;
    EnterCriticalSection(&mgr->lock);
    for (int i = 0; i < mgr->segment_count; i++)
        total += mgr->segments[i].downloaded;
    LeaveCriticalSection(&mgr->lock);
    return total;
}

/* ===== Dynamic thread adjustment ===== */

int segmgr_get_active_speeds(segment_manager_t* mgr, int64_t* speeds, int max_count) {
    int count = 0;
    EnterCriticalSection(&mgr->lock);
    for (int i = 0; i < mgr->segment_count && count < max_count; i++) {
        if (mgr->segments[i].state == SEG_DOWNLOADING)
            speeds[count++] = mgr->segments[i].speed_bps;
    }
    LeaveCriticalSection(&mgr->lock);

    /* Sort descending (simple bubble sort, small N) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (speeds[j] > speeds[i]) {
                int64_t t = speeds[i]; speeds[i] = speeds[j]; speeds[j] = t;
            }
        }
    }
    return count;
}

int64_t segmgr_get_baseline_speed(segment_manager_t* mgr) {
    int64_t speeds[8];
    int count = segmgr_get_active_speeds(mgr, speeds, 8);
    if (count == 0) return 0;

    /* Average of top 3 (or fewer if less active) */
    int n = (count < 3) ? count : 3;
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += speeds[i];
    return sum / n;
}

int segmgr_suspend_slowest(segment_manager_t* mgr) {
    EnterCriticalSection(&mgr->lock);

    int slowest_idx = -1;
    int64_t min_speed = INT64_MAX;

    for (int i = 0; i < mgr->segment_count; i++) {
        if (mgr->segments[i].state != SEG_DOWNLOADING) continue;
        /* Only suspend segments that have actually started downloading data */
        if (mgr->segments[i].downloaded == 0) continue;
        if (mgr->segments[i].speed_bps < min_speed) {
            min_speed = mgr->segments[i].speed_bps;
            slowest_idx = i;
        }
    }

    if (slowest_idx < 0) {
        LeaveCriticalSection(&mgr->lock);
        return -1;
    }

    /* Suspend: put remaining work back in queue */
    segment_t* s = &mgr->segments[slowest_idx];
    int64_t remaining = s->end_offset - s->start_offset + 1 - s->downloaded;
    if (remaining < 65536) {
        /* Too little remaining, let it finish */
        LeaveCriticalSection(&mgr->lock);
        return -1;
    }

    if (pending_enqueue_locked(mgr, s->index) != 0) {
        LeaveCriticalSection(&mgr->lock);
        return -1;
    }
    s->state = SEG_PENDING;
    mgr->active_count--;

    trace("Degrade: suspended slowest Seg(%d) speed=%lld, remaining=%lld",
          s->index, (long long)min_speed, (long long)remaining);

    LeaveCriticalSection(&mgr->lock);
    WakeConditionVariable(&mgr->cv_new_work);
    return 0;
}

int segmgr_retry_ready(segment_manager_t* mgr) {
    uint64_t now = GetTickCount64();
    int count = 0;
    EnterCriticalSection(&mgr->lock);
    for (int i = 0; i < mgr->segment_count; i++) {
        if (mgr->segments[i].state == SEG_SUSPENDED &&
            mgr->segments[i].suspend_until_ms <= now) {
            count++;
        }
    }
    LeaveCriticalSection(&mgr->lock);
    return count;
}

int segmgr_retry_expired(segment_manager_t* mgr) {
    uint64_t now = GetTickCount64();
    int count = 0;
    EnterCriticalSection(&mgr->lock);
    for (int i = 0; i < mgr->segment_count; i++) {
        segment_t* s = &mgr->segments[i];
        if (s->state == SEG_SUSPENDED && s->suspend_until_ms <= now) {
            if (pending_enqueue_locked(mgr, s->index) != 0) {
                mgr->fatal_error = true;
                break;
            }
            s->state = SEG_PENDING;
            count++;
        }
    }
    LeaveCriticalSection(&mgr->lock);
    if (count > 0) WakeAllConditionVariable(&mgr->cv_new_work);
    return count;
}

int64_t segmgr_total_remaining(segment_manager_t* mgr) {
    int64_t rem = 0;
    EnterCriticalSection(&mgr->lock);
    for (int i = 0; i < mgr->segment_count; i++) {
        segment_t* s = &mgr->segments[i];
        if (s->state != SEG_COMPLETE)
            rem += s->end_offset - s->start_offset + 1 - s->downloaded;
    }
    LeaveCriticalSection(&mgr->lock);
    return rem;
}

bool segmgr_has_error(segment_manager_t* mgr) {
    bool has_error = false;
    EnterCriticalSection(&mgr->lock);
    if (mgr->fatal_error)
        has_error = true;
    for (int i = 0; i < mgr->segment_count; i++) {
        if (mgr->segments[i].state == SEG_ERROR) {
            has_error = true;
            break;
        }
    }
    LeaveCriticalSection(&mgr->lock);
    return has_error;
}

void segmgr_abort(segment_manager_t* mgr) {
    EnterCriticalSection(&mgr->lock);
    mgr->fatal_error = true;
    LeaveCriticalSection(&mgr->lock);
    WakeAllConditionVariable(&mgr->cv_new_work);
}
