#ifndef PERSIST_H
#define PERSIST_H

#include <stdint.h>
#include <stdbool.h>
#include "segment.h"

/* segments.bin header magic: "MUDL" */
#define PERSIST_MAGIC 0x4D55444D
#define PERSIST_VERSION 2

/* Persist segment state to file.
 * Returns 0 on success, -1 on error. */
int persist_save(const char* path, segment_manager_t* mgr, int64_t file_size, int thread_count);

/* Load segment state from file.
 * Allocates and populates mgr->segments and mgr->pending_queue.
 * Returns 0 on success, -1 on error/file not found.
 * If thread_count is not NULL, receives the saved thread count. */
int persist_load(const char* path, segment_manager_t* mgr, int64_t file_size, int* thread_count);

/* Check if a resume file exists for the given download path */
bool persist_exists(const char* segfile_path);

/* Build segments.bin path from output file path */
void persist_path(const char* output_path, char* seg_path, int seg_path_n);

/* Remove resume file (on successful completion) */
void persist_remove(const char* path);

#endif /* PERSIST_H */
