/*
 * sdrgg_debug_log.h — USB/I2C transaction logger for libsdrgg
 *
 * Enable: export SDRGG_DEBUG_LOG=1 before starting dump1090
 * Log file: /tmp/sdrgg_debug.log (or SDRGG_DEBUG_LOG_PATH)
 */
#ifndef SDRGG_DEBUG_LOG_H
#define SDRGG_DEBUG_LOG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void sdrgg_debug_log_start(const char *path);
void sdrgg_debug_log_stop(void);
bool sdrgg_debug_log_active(void);

void sdrgg_debug_log_write(int slot, const char *dir, const char *block,
                           uint16_t reg, const uint8_t *data, int data_len,
                           int32_t rc, uint64_t start_us);

void sdrgg_debug_log_event(int slot, const char *event, const char *fmt, ...);

/* Get monotonic microseconds (for start_us parameter) */
static inline uint64_t sdrgg_debug_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

#ifdef __cplusplus
}
#endif

#endif
