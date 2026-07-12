#ifndef SEGMENT_H
#define SEGMENT_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

#define MIN_SEGMENT_SIZE (1024 * 1024)  /* 1MB minimum per segment */

typedef enum {
    SEG_PENDING,
    SEG_DOWNLOADING,
    SEG_COMPLETE,
    SEG_ERROR,
    SEG_SUSPENDED
} segment_state_t;

typedef struct {
    int      index;
    int64_t  start_offset;
    int64_t  end_offset;        /* inclusive */
    int64_t  downloaded;        /* bytes downloaded so far */
    segment_state_t state;
    int      retry_count;
    uint64_t suspend_until_ms;
    int      socket_fd;         /* owning thread id / socket */
    int64_t  speed_bps;
    uint64_t last_activity_ms;
    uint32_t crc32;
} segment_t;

typedef struct {
    segment_t* segments;
    int        segment_count;
    int        total_size;      /* sum of all segment byte sizes */
    int        active_count;
    int        complete_count;
    int        max_retries;
    bool       fatal_error;

    /* Pending queue (ring buffer of indices) */
    int*  pending_queue;
    int   pending_head;
    int   pending_tail;
    int   pending_capacity;

    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv_new_work;
    CONDITION_VARIABLE cv_done;

    int64_t file_size;
} segment_manager_t;

/* Initialize segment manager. Allocates segments for given file size. */
int segmgr_init(segment_manager_t* mgr, int64_t file_size, int max_connections);

/* Destroy and free */
void segmgr_destroy(segment_manager_t* mgr);

/* Get a pending segment for this thread. Returns NULL if none available. */
segment_t* segmgr_acquire_pending(segment_manager_t* mgr);

/* Split pending resume work before workers start, up to target_count. */
int segmgr_expand_pending(segment_manager_t* mgr, int target_count);

/* Mark segment as complete */
void segmgr_complete(segment_manager_t* mgr, segment_t* seg);

/* Report error on segment */
void segmgr_error(segment_manager_t* mgr, segment_t* seg);

/* Check if all segments are done */
bool segmgr_all_done(segment_manager_t* mgr);

/* Get total downloaded bytes across all segments */
int64_t segmgr_total_downloaded(segment_manager_t* mgr);

/* Get total bytes remaining */
int64_t segmgr_total_remaining(segment_manager_t* mgr);
/* Check whether any segment has permanently failed */
bool segmgr_has_error(segment_manager_t* mgr);
void segmgr_abort(segment_manager_t* mgr);
/* Get speeds of all active segments (for dynamic adjustment) */
int segmgr_get_active_speeds(segment_manager_t* mgr, int64_t* speeds, int max_count);

/* Get baseline speed: average of top 3 active segments */
int64_t segmgr_get_baseline_speed(segment_manager_t* mgr);

/* Suspend the slowest active segment and put its work back in queue */
int segmgr_suspend_slowest(segment_manager_t* mgr);

/* Count SUSPENDED segments that are ready to retry */
int segmgr_retry_ready(segment_manager_t* mgr);

/* Set a SUSPENDED segment back to PENDING if its timeout elapsed */
int segmgr_retry_expired(segment_manager_t* mgr);


#endif /* SEGMENT_H */
