#include "segment.h"
#include "persist.h"
#include "utils.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void assert_contiguous(const segment_manager_t* mgr) {
    int64_t next = 0;
    for (int i = 0; i < mgr->segment_count; i++) {
        const segment_t* s = &mgr->segments[i];
        assert(s->index == i);
        assert(s->start_offset == next);
        assert(s->end_offset >= s->start_offset);
        assert(s->downloaded >= 0);
        assert(s->downloaded <= s->end_offset - s->start_offset + 1);
        next = s->end_offset + 1;
    }
    assert(next == mgr->file_size);
}

static void test_initial_segments(void) {
    segment_manager_t mgr;
    assert(segmgr_init(&mgr, 32LL * 1024 * 1024, 8) == 0);
    assert(mgr.segment_count == 8);
    assert(mgr.pending_tail - mgr.pending_head + 1 == 8);
    assert_contiguous(&mgr);
    segmgr_destroy(&mgr);
}

static void test_expand_partial_resume(void) {
    segment_manager_t mgr;
    const int64_t size = 32LL * 1024 * 1024;
    const int64_t downloaded = 3LL * 1024 * 1024;
    assert(segmgr_init(&mgr, size, 1) == 0);
    mgr.segments[0].downloaded = downloaded;
    mgr.segments[0].crc32 = 1234;

    assert(segmgr_expand_pending(&mgr, 8) == 8);
    assert(mgr.segment_count == 8);
    assert(mgr.pending_tail - mgr.pending_head + 1 == 8);
    assert(segmgr_total_downloaded(&mgr) == downloaded);
    assert(mgr.segments[0].crc32 == 1234);
    assert_contiguous(&mgr);
    segmgr_destroy(&mgr);
}

static void test_completed_segments_are_not_requeued(void) {
    segment_manager_t mgr;
    assert(segmgr_init(&mgr, 32LL * 1024 * 1024, 2) == 0);
    int64_t first_len = mgr.segments[0].end_offset + 1;
    mgr.segments[0].state = SEG_COMPLETE;
    mgr.segments[0].downloaded = first_len;
    mgr.complete_count = 1;

    assert(segmgr_expand_pending(&mgr, 4) == 4);
    assert(mgr.complete_count == 1);
    assert(mgr.segments[0].state == SEG_COMPLETE);
    assert(segmgr_total_downloaded(&mgr) == first_len);
    for (int i = mgr.pending_head; i <= mgr.pending_tail; i++)
        assert(mgr.pending_queue[i] != 0);
    assert_contiguous(&mgr);
    segmgr_destroy(&mgr);
}

static void test_worker_limit_survives_retries(void) {
    segment_manager_t mgr;
    assert(segmgr_init(&mgr, 64LL * 1024 * 1024, 64) == 0);
    segmgr_set_worker_limit(&mgr, 32);

    segment_t* acquired[32];
    for (int i = 0; i < 32; i++) {
        acquired[i] = segmgr_acquire_pending(&mgr);
        assert(acquired[i] != NULL);
        assert(mgr.active_count == i + 1);
    }
    assert(segmgr_acquire_pending(&mgr) == NULL);
    assert(mgr.active_count == 32);

    for (int i = 0; i < 32; i++) {
        segmgr_error(&mgr, acquired[i]);
        acquired[i]->suspend_until_ms = 0;
    }
    assert(mgr.active_count == 0);
    assert(segmgr_retry_expired(&mgr) == 32);

    for (int i = 0; i < 32; i++)
        assert(segmgr_acquire_pending(&mgr) != NULL);
    assert(segmgr_acquire_pending(&mgr) == NULL);
    assert(mgr.active_count == 32);
    segmgr_destroy(&mgr);
}

static void test_idle_worker_splits_active_tail(void) {
    segment_manager_t mgr;
    assert(segmgr_init(&mgr, 64LL * 1024 * 1024, 4) == 0);

    segment_t* active[4];
    for (int i = 0; i < 4; i++) {
        active[i] = segmgr_acquire_pending(&mgr);
        assert(active[i] != NULL);
    }
    assert(segmgr_acquire_pending(&mgr) == NULL);

    segmgr_complete(&mgr, active[0]);
    assert(mgr.active_count == 3);
    int64_t old_end = active[1]->end_offset;
    assert(segmgr_split_largest_active(&mgr) == 1);
    assert(mgr.segment_count == 5);

    segment_t* stolen = segmgr_acquire_pending(&mgr);
    assert(stolen != NULL);
    assert(stolen->start_offset == active[1]->end_offset + 1);
    assert(stolen->end_offset == old_end);
    assert(stolen->downloaded == 0);
    assert(mgr.active_count == 4);

    segmgr_destroy(&mgr);
}

static void test_runtime_split_persists_in_offset_order(void) {
    const char* path = "test-runtime-split.segments";
    const char* validator = "etag:test-runtime-split";
    segment_manager_t mgr;
    assert(segmgr_init(&mgr, 64LL * 1024 * 1024, 4) == 0);
    for (int i = 0; i < 4; i++)
        assert(segmgr_acquire_pending(&mgr) != NULL);
    assert(segmgr_split_largest_active(&mgr) == 1);
    assert(persist_save(path, &mgr, mgr.file_size, 4, validator) == 0);

    segment_manager_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    assert(persist_load(path, &loaded, mgr.file_size, NULL, validator) == 0);
    assert(loaded.segment_count == 5);
    assert_contiguous(&loaded);

    segmgr_destroy(&loaded);
    segmgr_destroy(&mgr);
    persist_remove(path);
}

static void test_crc32_combine(void) {
    const char first[] = "resume ";
    const char second[] = "download";
    char joined[32];
    snprintf(joined, sizeof(joined), "%s%s", first, second);

    uint32_t first_crc = crc32_update(0, first, (int)strlen(first));
    uint32_t second_crc = crc32_update(0, second, (int)strlen(second));
    uint32_t joined_crc = crc32_update(0, joined, (int)strlen(joined));
    assert(crc32_combine(first_crc, second_crc,
                         (int64_t)strlen(second)) == joined_crc);
}

static void test_compact_completed_prefix_before_expand(void) {
    segment_manager_t mgr;
    assert(segmgr_init(&mgr, 32LL * 1024 * 1024, 4) == 0);

    int64_t expected_downloaded = 0;
    uint32_t expected_crc = 0;
    for (int i = 0; i < 3; i++) {
        segment_t* segment = &mgr.segments[i];
        int64_t length = segment->end_offset - segment->start_offset + 1;
        segment->state = SEG_COMPLETE;
        segment->downloaded = length;
        segment->crc32 = (uint32_t)(100 + i);
        expected_crc = crc32_combine(expected_crc, segment->crc32, length);
        expected_downloaded += length;
    }
    mgr.complete_count = 3;

    mgr.segments[3].downloaded = 256 * 1024;
    mgr.segments[3].crc32 = 999;
    expected_crc = crc32_combine(expected_crc, mgr.segments[3].crc32,
                                 mgr.segments[3].downloaded);
    expected_downloaded += mgr.segments[3].downloaded;

    assert(segmgr_compact_verified(&mgr) == 3);
    assert(mgr.segment_count == 1);
    assert(mgr.complete_count == 0);
    assert(mgr.segments[0].state == SEG_PENDING);
    assert(mgr.segments[0].downloaded == expected_downloaded);
    assert(mgr.segments[0].crc32 == expected_crc);
    assert_contiguous(&mgr);

    assert(segmgr_expand_pending(&mgr, 4) == 4);
    assert(mgr.segment_count == 4);
    assert(segmgr_total_downloaded(&mgr) == expected_downloaded);
    assert_contiguous(&mgr);
    segmgr_destroy(&mgr);
}

int main(void) {
    test_initial_segments();
    test_expand_partial_resume();
    test_completed_segments_are_not_requeued();
    test_worker_limit_survives_retries();
    test_idle_worker_splits_active_tail();
    test_runtime_split_persists_in_offset_order();
    test_crc32_combine();
    test_compact_completed_prefix_before_expand();
    puts("segment unit tests passed");
    return 0;
}
