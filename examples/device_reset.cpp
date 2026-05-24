/*
 * device_reset.cpp - Multi-level device reset demonstration
 *
 * Shows how to use the reset:: namespace API to recover a device
 * from various failure modes without restarting the application.
 *
 * Reset levels (lightest to heaviest):
 *   Level 1: reset_demod   - DSP pipeline soft reset (sub-ms)
 *   Level 2: reset_tuner   - Tuner shadow register writeback
 *   Level 3: reset_usb     - USB bus reset (device re-enumerates)
 *   Level 4: reset_power   - USB port power cycle (cold reset)
 *   Full:    reset_full    - Levels 1+2+3 in sequence
 *
 * Usage:
 *   sudo ./examples/device_reset [level]
 *
 *   level: 1, 2, 3, 4, or "full" (default: full)
 *
 * License: MIT
 * Created by Luigi Origa.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "../sdrgg.h"

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [level]\n", argv0);
    fprintf(stderr, "  level: 1 (demod), 2 (tuner), 3 (usb), 4 (power), full (1+2+3)\n");
    fprintf(stderr, "  Default: full\n");
}

int main(int argc, char *argv[]) {
    int level = 0; // 0 = full
    if (argc > 1) {
        if (strcmp(argv[1], "full") == 0) level = 0;
        else if (strcmp(argv[1], "1") == 0) level = 1;
        else if (strcmp(argv[1], "2") == 0) level = 2;
        else if (strcmp(argv[1], "3") == 0) level = 3;
        else if (strcmp(argv[1], "4") == 0) level = 4;
        else { print_usage(argv[0]); return 1; }
    }

    /* Create context and enumerate */
    sdrgg_ctx_t *ctx = sdr::create();
    if (!ctx) {
        fprintf(stderr, "ERROR: sdr::create() failed\n");
        return 1;
    }

    sdrgg_devinfo_t devs[4];
    int32_t count = sdrgg_enumerate(ctx, devs, 4);
    if (count <= 0) {
        fprintf(stderr, "ERROR: no RTL2832U devices found\n");
        sdr::destroy(ctx);
        return 1;
    }

    printf("Found %d device(s). Using device 0: SN=%s tuner=%d\n",
           count, devs[0].serial, devs[0].tuner);

    /* Open the first device */
    sdrgg_dev_t *dev = sdr::open(ctx, 0);
    if (!dev) {
        fprintf(stderr, "ERROR: sdr::open(0) failed\n");
        sdr::destroy(ctx);
        return 1;
    }

    /* Configure device to a valid state first */
    uint32_t actual_freq = 0, actual_rate = 0;
    sdr::set_frequency(dev, 1090000000, &actual_freq);
    sdr::set_sample_rate(dev, 2400000, &actual_rate);
    sdr::set_gain(dev, 496); // 49.6 dB
    printf("Configured: freq=%u rate=%u\n", actual_freq, actual_rate);

    /* Perform the requested reset level */
    int32_t rc;
    switch (level) {
    case 1:
        printf("Performing Level 1: demod soft reset...\n");
        rc = reset::reset_demod(dev);
        printf("  reset_demod() returned %d (%s)\n", rc, rc == SDRGG_OK ? "OK" : "FAILED");
        break;

    case 2:
        printf("Performing Level 2: tuner shadow writeback...\n");
        rc = reset::reset_tuner(dev);
        printf("  reset_tuner() returned %d (%s)\n", rc, rc == SDRGG_OK ? "OK" : "FAILED");
        break;

    case 3:
        printf("Performing Level 3: USB device reset...\n");
        printf("  WARNING: device will re-enumerate, fd becomes invalid\n");
        rc = reset::reset_usb(dev);
        printf("  reset_usb() returned %d (%s)\n", rc, rc == SDRGG_OK ? "OK" : "FAILED");
        if (rc == SDRGG_OK) {
            printf("  Device must be re-opened after USB reset.\n");
        }
        break;

    case 4:
        printf("Performing Level 4: USB port power cycle...\n");
        printf("  WARNING: device will power off for 2s, then re-enumerate\n");
        rc = reset::reset_power(dev, NULL);
        printf("  reset_power() returned %d (%s)\n", rc, rc == SDRGG_OK ? "OK" : "FAILED");
        if (rc == SDRGG_OK) {
            printf("  Device must be completely re-opened after power cycle.\n");
        }
        break;

    default: /* full */
        printf("Performing Full reset (levels 1+2+3)...\n");
        rc = reset::reset_full(dev);
        printf("  reset_full() returned %d (%s)\n", rc, rc == SDRGG_OK ? "OK" : "FAILED");
        if (rc == SDRGG_OK) {
            printf("  Device must be re-opened after full reset.\n");
        }
        break;
    }

    /* Cleanup — for levels 3/4/full, close is best-effort since fd may be invalid */
    sdr::close(dev);
    sdr::destroy(ctx);

    return (rc == SDRGG_OK) ? 0 : 1;
}
