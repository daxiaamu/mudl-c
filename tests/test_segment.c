#include "segment.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

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

int main(void) {
    test_initial_segments();
    test_expand_partial_resume();
    test_completed_segments_are_not_requeued();
    puts("segment unit tests passed");
    return 0;
}
