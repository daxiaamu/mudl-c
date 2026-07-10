#include "segment.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int segmgr_init(segment_manager_t* mgr, int64_t file_size, int max_connections) {
    memset(mgr, 0, sizeof(segment_manager_t));
    mgr->file_size = file_size;
    mgr->max_retries = 5;

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

segment_t* segmgr_try_steal(segment_manager_t* mgr, int thread_id) {
    EnterCriticalSection(&mgr->lock);

    segment_t* best = NULL;
    int64_t max_remaining = 0;

    for (int i = 0; i < mgr->segment_count; i++) {
        segment_t* s = &mgr->segments[i];
        if (s->state != SEG_DOWNLOADING) continue;

        int64_t remaining = s->end_offset - s->start_offset + 1 - s->downloaded;
        if (remaining < ROLLBACK_THRESHOLD) continue;

        /* Estimate completion time */
        if (s->speed_bps > 0) {
            int64_t est_ms = remaining * 1000 / s->speed_bps;
            if (est_ms < 500) continue;  /* too fast, don't bother */
        }

        if (remaining > max_remaining) {
            max_remaining = remaining;
            best = s;
        }
    }

    if (!best) {
        LeaveCriticalSection(&mgr->lock);
        return NULL;
    }

    /* RollBack: split best segment, create new work */
    int64_t old_end = best->end_offset;
    int64_t steal_size = max_remaining / 2;
    int64_t steal_start = best->end_offset - steal_size + 1;
    best->end_offset -= steal_size;

    /* Need a new segment slot. Resize if necessary. */
    int new_idx = mgr->segment_count;
    int new_count = mgr->segment_count + 1;
    segment_t* new_segs = (segment_t*)realloc(mgr->segments, new_count * sizeof(segment_t));
    if (!new_segs) {
        best->end_offset = old_end; /* rollback the rollback */
        LeaveCriticalSection(&mgr->lock);
        return NULL;
    }
    mgr->segments = new_segs;

    /* Grow pending queue if needed */
    if (mgr->pending_tail >= mgr->pending_capacity - 1) {
        mgr->pending_capacity += 4;
        mgr->pending_queue = (int*)realloc(mgr->pending_queue,
                                           mgr->pending_capacity * sizeof(int));
    }

    segment_t* new_seg = &mgr->segments[new_idx];
    memset(new_seg, 0, sizeof(segment_t));
    new_seg->index = new_idx;
    new_seg->start_offset = steal_start;
    new_seg->end_offset = steal_start + steal_size - 1;
    new_seg->downloaded = 0;
    new_seg->state = SEG_PENDING;
    mgr->segment_count = new_count;
    mgr->pending_queue[++mgr->pending_tail] = new_idx;

    trace("RollBack: Thread(%d) stole %lld bytes from Seg(%d) -> new Seg(%d) [%lld-%lld]",
          thread_id, (long long)steal_size, best->index, new_idx,
          (long long)new_seg->start_offset, (long long)new_seg->end_offset);

    LeaveCriticalSection(&mgr->lock);
    WakeConditionVariable(&mgr->cv_new_work);
    return new_seg;
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

    s->state = SEG_PENDING;
    mgr->pending_queue[++mgr->pending_tail] = s->index;
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
            s->state = SEG_PENDING;
            mgr->pending_queue[++mgr->pending_tail] = s->index;
            count++;
        }
    }
    LeaveCriticalSection(&mgr->lock);
    if (count > 0) WakeConditionVariable(&mgr->cv_new_work);
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
