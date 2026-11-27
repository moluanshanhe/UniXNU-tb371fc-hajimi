/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB371FC_TOUCH_REPORT_H
#define TB371FC_TOUCH_REPORT_H

#include <stddef.h>
#include <stdint.h>

#define TB371FC_TOUCH_PAYLOAD_BYTES 108u
#define TB371FC_TOUCH_REPORT_BYTES (TB371FC_TOUCH_PAYLOAD_BYTES + 1u)
#define TB371FC_TOUCH_CONTACTS 10u
#define TB371FC_TOUCH_RAW_X_MAX 18400u
#define TB371FC_TOUCH_RAW_Y_MAX 29440u
#define TB371FC_HID_INPUT_X_MAX 30480u
#define TB371FC_HID_INPUT_Y_MAX 20320u

typedef struct {
  uint8_t id;
  uint16_t x;
  uint16_t y;
  uint8_t width;
  uint8_t pressure;
} Tb371fcTouchContact;

typedef struct {
  uint8_t count;
  Tb371fcTouchContact contacts[TB371FC_TOUCH_CONTACTS];
} Tb371fcTouchFrame;

typedef struct {
  uint16_t x_min;
  uint16_t x_max;
  uint16_t y_min;
  uint16_t y_max;
  uint8_t swap_xy;
  uint8_t flip_x;
  uint8_t flip_y;
} Tb371fcTouchCal;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 for a valid 16-bit 10x super-res frame (18400x29440) and <0 on error. */
int tb371fc_touch_decode(const uint8_t *report, size_t length,
                         Tb371fcTouchFrame *frame);
/* Decode using firmware-reported coordinate bounds: uses standard Novatek
 * 12-bit packed slots when abs_x_max <= 4095 && abs_y_max <= 4095, and
 * 16-bit big-endian slots for 10x super-res 18400x29440. */
int tb371fc_touch_decode_ex(const uint8_t *report, size_t length,
                            uint16_t abs_x_max, uint16_t abs_y_max,
                            Tb371fcTouchFrame *frame);

/* Reports include one SPI dummy byte before the 108-byte Novatek payload. */
int tb371fc_touch_checksum_ok(const uint8_t *report, size_t length);
/* 0 = point report, 1 = firmware handshake, 2 = watchdog recovery. */
int tb371fc_touch_recovery(const uint8_t *report, size_t length);

/* Convert native 18400x29440 coordinates to NativeHidInput landscape ranges. */
void tb371fc_touch_to_hid(uint16_t native_x, uint16_t native_y,
                          uint16_t *hid_x, uint16_t *hid_y);

/* Convert raw touch coordinates using an explicit calibration window & orientation. */
void tb371fc_touch_map_calibrated(uint16_t raw_x, uint16_t raw_y,
                                  const Tb371fcTouchCal *cal,
                                  uint16_t *hid_x, uint16_t *hid_y);

#ifdef __cplusplus
}
#endif

#endif
