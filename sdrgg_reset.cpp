/*
 * sdrgg_reset.cpp - Multi-level device reset operations
 *
 * Provides four reset levels:
 *   Level 1: RTL2832U demodulator soft reset (DSP pipeline only)
 *   Level 2: Tuner register re-initialization from shadow file
 *   Level 3: USB device reset (ioctl USBDEVFS_RESET)
 *   Level 4: USB port power cycle via sysfs (true cold reset)
 *
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <dirent.h>

#include "sdrgg_internal.h"

/* ======================================================================
 *  Level 1: RTL2832U demodulator soft reset
 *
 *  Toggles bit 2 of demod page 1, register 0x01.
 *  Resets the DSP pipeline (decimation filters, AGC accumulators)
 *  without disturbing the tuner or USB link.
 * ====================================================================== */

int32_t reset::reset_demod( sdrgg_dev_t *dev ) {
  if( !dev ) return SDRGG_ERR_PARAM;

  uint8_t val;
  int32_t rc;

  /* Read current value of page1 reg 0x01 */
  rc = demod::read( dev, SDRGG_BLOCK_DEMOD, 0x0101, &val );
  if( rc != SDRGG_OK ) return rc;

  /* Set bit 2 (soft reset) */
  rc = demod::write( dev, SDRGG_BLOCK_DEMOD, 0x0101, val | 0x04 );
  if( rc != SDRGG_OK ) return rc;

  usleep( 1000 ); /* 1 ms settle */

  /* Clear bit 2 (release reset) */
  rc = demod::write( dev, SDRGG_BLOCK_DEMOD, 0x0101, val & ~0x04 );
  if( rc != SDRGG_OK ) return rc;

  usleep( 1000 ); /* 1 ms settle */

  return SDRGG_OK;
}

/* ======================================================================
 *  Level 2: Tuner register re-initialization from shadow
 *
 *  Re-writes all tuner registers from the internal shadow file.
 *  Fixes I2C state corruption without touching USB or demod.
 *  Only supported for R820T tuner family.
 * ====================================================================== */

int32_t reset::reset_tuner( sdrgg_dev_t *dev ) {
  if( !dev ) return SDRGG_ERR_PARAM;

  if( dev->identity.tuner_class != SDRGG_TUNER_R820T ) {
    /* For non-R820T tuners, re-init via public API */
    if( dev->identity.tuner_class == SDRGG_TUNER_FC0012 ) {
      return fc0012::init( dev );
    }
    return SDRGG_ERR_PARAM;
  }

  /* Enable I2C repeater for tuner access */
  int32_t rc = rtl::enable_i2c_repeater( dev, true );
  if( rc != SDRGG_OK ) return rc;

  /* Re-write all R820T registers from shadow */
  for( int32_t i = 0; i < SDRGG_R820T_NUM_REGS; i++ ) {
    rc = tuner::write_reg( dev, SDRGG_R820T_REG_START + (uint8_t)i,
                           dev->shadow.r820t_file[i] );
    if( rc != SDRGG_OK ) {
      rtl::enable_i2c_repeater( dev, false );
      return rc;
    }
  }

  rtl::enable_i2c_repeater( dev, false );
  return SDRGG_OK;
}

/* ======================================================================
 *  Level 3: USB device reset (ioctl USBDEVFS_RESET)
 *
 *  Issues a USB bus-level reset. The device re-enumerates on the same
 *  port but does NOT power-cycle. The device fd becomes invalid after
 *  this call — the caller must close and re-open the device.
 * ====================================================================== */

int32_t reset::reset_usb( sdrgg_dev_t *dev ) {
  if( !dev ) return SDRGG_ERR_PARAM;

  int32_t fd = dev->identity.fd;
  if( fd < 0 ) return SDRGG_ERR_USB;

  int32_t ret = ioctl( fd, USBDEVFS_RESET, NULL );
  if( ret < 0 ) {
    fprintf( stderr, "sdrgg-reset: USBDEVFS_RESET failed: %s\n", strerror( errno ) );
    return SDRGG_ERR_USB;
  }

  /* Brief settle time for USB re-enumeration */
  usleep( 500000 ); /* 500 ms */

  return SDRGG_OK;
}

/* ======================================================================
 *  Level 4: USB port power cycle via sysfs
 *
 *  Writes '0' then '1' to /sys/bus/usb/devices/<path>/authorized
 *  which cuts VBUS power to the port. This is the equivalent of a
 *  physical unplug/replug cycle.
 *
 *  Requires root privileges (or appropriate udev rules).
 *  After this call the device fd is invalid and the device must be
 *  completely re-enumerated and re-opened.
 * ====================================================================== */

/* Try to resolve the sysfs USB path from the device fd */
static int32_t resolve_usb_path( int32_t fd, char *path_buf, size_t buf_len ) {
  char fd_link[64];
  char dev_path[256];
  ssize_t len;

  snprintf( fd_link, sizeof( fd_link ), "/proc/self/fd/%d", fd );
  len = readlink( fd_link, dev_path, sizeof( dev_path ) - 1 );
  if( len <= 0 ) return SDRGG_ERR_USB;
  dev_path[len] = '\0';

  /* dev_path is like /dev/bus/usb/001/004
   * We need to find the corresponding sysfs path.
   * Parse bus and devnum, then scan /sys/bus/usb/devices/ */
  int bus = 0, devnum = 0;
  if( sscanf( dev_path, "/dev/bus/usb/%d/%d", &bus, &devnum ) != 2 ) {
    return SDRGG_ERR_USB;
  }

  /* Scan sysfs USB devices to find matching busnum/devnum */
  DIR *dir = opendir( "/sys/bus/usb/devices" );
  if( !dir ) return SDRGG_ERR_USB;

  struct dirent *ent;
  while( ( ent = readdir( dir ) ) != NULL ) {
    /* Skip entries that start with usb (root hubs) or contain ':' (interfaces) */
    if( ent->d_name[0] == '.' ) continue;
    if( strchr( ent->d_name, ':' ) ) continue;
    if( strncmp( ent->d_name, "usb", 3 ) == 0 ) continue;

    char attr_path[320];
    char attr_val[16];
    int32_t attr_fd;
    ssize_t n;

    /* Check busnum */
    snprintf( attr_path, sizeof( attr_path ),
              "/sys/bus/usb/devices/%s/busnum", ent->d_name );
    attr_fd = open( attr_path, O_RDONLY );
    if( attr_fd < 0 ) continue;
    n = read( attr_fd, attr_val, sizeof( attr_val ) - 1 );
    close( attr_fd );
    if( n <= 0 ) continue;
    attr_val[n] = '\0';
    if( atoi( attr_val ) != bus ) continue;

    /* Check devnum */
    snprintf( attr_path, sizeof( attr_path ),
              "/sys/bus/usb/devices/%s/devnum", ent->d_name );
    attr_fd = open( attr_path, O_RDONLY );
    if( attr_fd < 0 ) continue;
    n = read( attr_fd, attr_val, sizeof( attr_val ) - 1 );
    close( attr_fd );
    if( n <= 0 ) continue;
    attr_val[n] = '\0';
    if( atoi( attr_val ) != devnum ) continue;

    /* Found it */
    size_t name_len = strnlen( ent->d_name, buf_len );
    if( name_len >= buf_len ) continue;
    memcpy( path_buf, ent->d_name, name_len );
    path_buf[name_len] = '\0';
    closedir( dir );
    return SDRGG_OK;
  }

  closedir( dir );
  return SDRGG_ERR_USB;
}

int32_t reset::reset_power( sdrgg_dev_t *dev, const char *usb_path ) {
  char resolved_path[64];

  if( !dev && !usb_path ) return SDRGG_ERR_PARAM;

  if( !usb_path ) {
    /* Try to resolve from device fd */
    if( dev->identity.fd < 0 ) return SDRGG_ERR_USB;
    int32_t rc = resolve_usb_path( dev->identity.fd, resolved_path, sizeof( resolved_path ) );
    if( rc != SDRGG_OK ) return rc;
    usb_path = resolved_path;
  }

  char sysfs_path[256];
  snprintf( sysfs_path, sizeof( sysfs_path ),
            "/sys/bus/usb/devices/%s/authorized", usb_path );

  /* Deauthorize (power off) */
  int32_t fd = open( sysfs_path, O_WRONLY );
  if( fd < 0 ) {
    fprintf( stderr, "sdrgg-reset: cannot open %s: %s\n", sysfs_path, strerror( errno ) );
    return SDRGG_ERR_USB;
  }

  if( write( fd, "0", 1 ) != 1 ) {
    close( fd );
    return SDRGG_ERR_USB;
  }
  close( fd );

  /* Hold power off for 2 seconds */
  usleep( 2000000 );

  /* Re-authorize (power on) */
  fd = open( sysfs_path, O_WRONLY );
  if( fd < 0 ) {
    fprintf( stderr, "sdrgg-reset: cannot re-open %s: %s\n", sysfs_path, strerror( errno ) );
    return SDRGG_ERR_USB;
  }

  if( write( fd, "1", 1 ) != 1 ) {
    close( fd );
    return SDRGG_ERR_USB;
  }
  close( fd );

  /* Wait for device re-enumeration */
  usleep( 3000000 ); /* 3 seconds */

  return SDRGG_OK;
}

/* ======================================================================
 *  Convenience: Full reset sequence (levels 1+2+3)
 *
 *  Attempts demod reset, then tuner re-init, then USB reset.
 *  Stops at the first failure. Does NOT include power cycle (level 4)
 *  because that requires the device to be completely re-opened.
 * ====================================================================== */

int32_t reset::reset_full( sdrgg_dev_t *dev ) {
  if( !dev ) return SDRGG_ERR_PARAM;

  int32_t rc;

  /* Level 1: demod soft reset */
  rc = reset_demod( dev );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "sdrgg-reset: demod reset failed (%d), continuing...\n", rc );
  }

  /* Level 2: tuner re-init */
  rc = reset_tuner( dev );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "sdrgg-reset: tuner reset failed (%d), escalating to USB...\n", rc );
  }

  /* Level 3: USB device reset */
  rc = reset_usb( dev );
  return rc;
}
