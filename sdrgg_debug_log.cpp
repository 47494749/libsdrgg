/*
 * sdrgg_debug_log.cpp — USB/I2C transaction logger for libsdrgg
 *
 * Logs every register read/write, tuner I2C operation, and bulk transfer
 * with microsecond timestamps. Enabled via SDRGG_DEBUG_LOG=1 env var
 * or sdrgg_debug_log_start(). Writes to /tmp/sdrgg_debug.log.
 *
 * Each line: timestamp_us | slot | dir | block | reg | data_hex | rc | latency_us | caller_tid
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/syscall.h>

#include "sdrgg_internal.h"
#include "sdrgg_debug_log.h"

static FILE *g_log_fp = NULL;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_log_enabled = false;

static uint64_t log_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static const char *block_name(uint8_t block) {
    switch (block) {
        case SDRGG_BLOCK_DEMOD: return "DEMOD";
        case SDRGG_BLOCK_USB:   return "USB";
        case SDRGG_BLOCK_SYS:   return "SYS";
        case SDRGG_BLOCK_TUNER: return "TUNER";
        default: return "?";
    }
}

/* All public functions use C linkage (declared extern "C" in header) */

extern "C" void sdrgg_debug_log_start(const char *path) {
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    const char *p = path ? path : "/tmp/sdrgg_debug.log";
    g_log_fp = fopen(p, "a");
    if (g_log_fp) {
        g_log_enabled = true;
        fprintf(g_log_fp, "# sdrgg debug log started at %lu us\n", (unsigned long)log_time_us());
        fprintf(g_log_fp, "# timestamp_us | slot | dir | block | reg | data | rc | latency_us | tid\n");
        fflush(g_log_fp);
        fprintf(stderr, "sdrgg: debug log enabled -> %s\n", p);
    }
    pthread_mutex_unlock(&g_log_lock);
}

extern "C" void sdrgg_debug_log_stop(void) {
    pthread_mutex_lock(&g_log_lock);
    g_log_enabled = false;
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    pthread_mutex_unlock(&g_log_lock);
}

extern "C" bool sdrgg_debug_log_active(void) {
    return g_log_enabled;
}

extern "C" void sdrgg_debug_log_write(int slot, const char *dir, const char *block,
                           uint16_t reg, const uint8_t *data, int data_len,
                           int32_t rc, uint64_t start_us) {
    if (!g_log_enabled || !g_log_fp) return;
    uint64_t now = log_time_us();
    uint64_t latency = now - start_us;
    long tid = syscall(SYS_gettid);

    char hex[128] = {};
    int hlen = 0;
    for (int i = 0; i < data_len && hlen < 120; i++)
        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X", data[i]);

    pthread_mutex_lock(&g_log_lock);
    fprintf(g_log_fp, "%lu | %d | %s | %-5s | 0x%04X | %s | %d | %lu | %ld\n",
            (unsigned long)now, slot, dir, block, reg,
            hex[0] ? hex : "-", rc, (unsigned long)latency, tid);
    fflush(g_log_fp);
    pthread_mutex_unlock(&g_log_lock);
}

extern "C" void sdrgg_debug_log_event(int slot, const char *event, const char *fmt, ...) {
    if (!g_log_enabled || !g_log_fp) return;
    uint64_t now = log_time_us();
    long tid = syscall(SYS_gettid);

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_lock);
    fprintf(g_log_fp, "%lu | %d | EVENT | %-5s | %s | %ld\n",
            (unsigned long)now, slot, event, msg, tid);
    fflush(g_log_fp);
    pthread_mutex_unlock(&g_log_lock);
}

/* Auto-start from environment variable */
__attribute__((constructor))
static void sdrgg_debug_log_auto_init(void) {
    const char *env = getenv("SDRGG_DEBUG_LOG");
    if (env && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')) {
        const char *path = getenv("SDRGG_DEBUG_LOG_PATH");
        sdrgg_debug_log_start(path);
    }
}
