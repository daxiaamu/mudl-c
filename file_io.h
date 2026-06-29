#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

/* File handle wrapper */
typedef struct {
    HANDLE      hFile;
    char        path[MAX_PATH];
    int64_t     size;
    bool        opened;
    char        last_error[256];
    CRITICAL_SECTION lock;
} file_t;

/* Open file for writing. Creates dirs if needed. */
int file_open(file_t* f, const char* path, int64_t expected_size);

/* Write data at specific offset (for multi-threaded access) */
int file_write_at(file_t* f, int64_t offset, const char* data, int len);

/* Write data sequentially (simpler, for single thread) */
int file_write(file_t* f, const char* data, int len);

/* Close file */
void file_close(file_t* f);

/* Check if file exists */
bool file_exists(const char* path);

/* Get file size */
int64_t file_size(const char* path);

/* Generate output filename from URL */
int file_name_from_url(const char* url, char* name, int name_n,
                       const char* content_disposition);

/* Generate safe path (handles \\?\ for long paths) */
int file_make_safe_path(const char* dir, const char* name,
                        char* out, int out_n);

#endif /* FILE_IO_H */
