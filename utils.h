#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdbool.h>

/* Debug trace (only prints if MUDM_DEBUG env var is set) */
void trace(const char* fmt, ...);

/* Time helpers */
uint64_t now_ms(void);
void sleep_ms(uint32_t ms);

/* String helpers */
char* str_trim(char* s);
char* str_join_path(const char* dir, const char* file);
bool str_ends_with(const char* s, const char* suffix);

/* Number formatting - returns static buf */
const char* fmt_bytes(int64_t bytes);
const char* fmt_speed(int64_t bps);
const char* fmt_pct(double pct);

/* Error handling */
void die(const char* fmt, ...);
void warn(const char* fmt, ...);

/* Checksum helpers */
uint32_t crc32_update(uint32_t crc, const void* data, int len);
uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, int64_t len2);

/* Console */
bool console_has_color(void);

#endif /* UTILS_H */
