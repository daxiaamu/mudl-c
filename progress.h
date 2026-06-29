#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PROGRESS_BAR = 0,
    PROGRESS_JSON,
    PROGRESS_SILENT,
} progress_mode_t;

typedef struct {
    progress_mode_t mode;
    int64_t         downloaded;
    int64_t         total;
    int64_t         speed_bps;
    int             threads_active;
    int             threads_total;
    uint64_t        start_time_ms;
    bool            finished;
} progress_t;

void progress_init(progress_t* p, progress_mode_t mode,
                   int64_t total, int threads);
void progress_update(progress_t* p, int64_t downloaded,
                     int64_t speed, int active, int total);
void progress_done(progress_t* p);
void progress_log(progress_t* p, const char* msg);

#endif /* PROGRESS_H */
