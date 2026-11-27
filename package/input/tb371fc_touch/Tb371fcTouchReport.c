/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb371fcTouchReport.h"

#define TB371FC_TOUCH_X_MAX TB371FC_TOUCH_RAW_X_MAX
#define TB371FC_TOUCH_Y_MAX TB371FC_TOUCH_RAW_Y_MAX

int
tb371fc_touch_checksum_ok(const uint8_t *report, size_t length)
{
  uint8_t sum = 0;
  unsigned i;

  if (report == 0 || length < TB371FC_TOUCH_REPORT_BYTES)
    return 0;
  for (i = 1; i < 65u; ++i)
    sum = (uint8_t)(sum + report[i]);
  return (uint8_t)(0u - sum) == report[65];
}

int
tb371fc_touch_recovery(const uint8_t *report, size_t length)
{
  unsigned i;
  int handshake = 1, watchdog = 1;

  if (report == 0 || length < TB371FC_TOUCH_REPORT_BYTES)
    return 0;
  for (i = 1; i < 7u; ++i) {
    if (report[i] != 0x77u)
      handshake = 0;
    if (report[i] != 0xfdu && report[i] != 0xfeu)
      watchdog = 0;
  }
  return handshake ? 1 : watchdog ? 2 : 0;
}

int
tb371fc_touch_decode_ex(const uint8_t *report, size_t length,
                        uint16_t abs_x_max, uint16_t abs_y_max,
                        Tb371fcTouchFrame *frame)
{
  uint16_t seen = 0;
  unsigned slot;
  const int packed12 = (abs_x_max > 0u && abs_y_max > 0u &&
                        abs_x_max <= 4095u && abs_y_max <= 4095u);
  const uint16_t x_lim = abs_x_max ? abs_x_max : TB371FC_TOUCH_X_MAX;
  const uint16_t y_lim = abs_y_max ? abs_y_max : TB371FC_TOUCH_Y_MAX;

  if (report == 0 || frame == 0 || length < TB371FC_TOUCH_REPORT_BYTES)
    return -1;
  if (tb371fc_touch_recovery(report, length) != 0)
    return -2;
  if (!tb371fc_touch_checksum_ok(report, length))
    return -1;

  frame->count = 0;
  for (slot = 0; slot < TB371FC_TOUCH_CONTACTS; ++slot) {
    const size_t pos = 1u + 6u * slot;
    const uint8_t state_and_id = report[pos];
    const uint8_t id = state_and_id >> 3;
    const uint8_t state = state_and_id & 7u;
    uint16_t x, y;
    uint8_t width, pressure;
    Tb371fcTouchContact *contact;

    if (state != 1u && state != 2u)
      continue;
    if (id == 0u || id > TB371FC_TOUCH_CONTACTS ||
        (seen & (uint16_t)(1u << (id - 1u))) != 0u)
      return -1;

    if (packed12) {
      x = (uint16_t)(((uint16_t)report[pos + 1u] << 4) | (report[pos + 3u] >> 4));
      y = (uint16_t)(((uint16_t)report[pos + 2u] << 4) | (report[pos + 3u] & 0x0Fu));
      width = report[pos + 4u] ? report[pos + 4u] : 1u;
      pressure = report[pos + 5u] ? report[pos + 5u] : 1u;
    } else {
      x = (uint16_t)(((uint16_t)report[pos + 1u] << 8) | report[pos + 2u]);
      y = (uint16_t)(((uint16_t)report[pos + 3u] << 8) | report[pos + 4u]);
      width = report[pos + 5u] ? report[pos + 5u] : 1u;
      pressure = report[99u + slot] ? report[99u + slot] : 1u;
    }
    if (x > x_lim || y > y_lim)
      return -1;

    seen |= (uint16_t)(1u << (id - 1u));
    contact = &frame->contacts[frame->count++];
    contact->id = id;
    contact->x = x;
    contact->y = y;
    contact->width = width;
    contact->pressure = pressure;
  }
  return 0;
}

int
tb371fc_touch_decode(const uint8_t *report, size_t length,
                     Tb371fcTouchFrame *frame)
{
  return tb371fc_touch_decode_ex(report, length,
                                 TB371FC_TOUCH_X_MAX, TB371FC_TOUCH_Y_MAX,
                                 frame);
}

void
tb371fc_touch_map_calibrated(uint16_t raw_x, uint16_t raw_y,
                             const Tb371fcTouchCal *cal,
                             uint16_t *hid_x, uint16_t *hid_y)
{
  static const Tb371fcTouchCal default_cal = {
    0u, TB371FC_TOUCH_X_MAX, 0u, TB371FC_TOUCH_Y_MAX, 1u, 0u, 1u
  };
  const Tb371fcTouchCal *c = cal ? cal : &default_cal;
  const uint32_t x_min = c->x_min;
  const uint32_t x_max = (c->x_max > c->x_min) ? c->x_max : (c->x_min + 1u);
  const uint32_t y_min = c->y_min;
  const uint32_t y_max = (c->y_max > c->y_min) ? c->y_max : (c->y_min + 1u);
  const uint32_t x_span = x_max - x_min;
  const uint32_t y_span = y_max - y_min;
  uint32_t cx = raw_x < x_min ? 0u : (raw_x > x_max ? x_span : (uint32_t)raw_x - x_min);
  uint32_t cy = raw_y < y_min ? 0u : (raw_y > y_max ? y_span : (uint32_t)raw_y - y_min);
  uint32_t h_val = c->swap_xy ? cy : cx;
  uint32_t h_span = c->swap_xy ? y_span : x_span;
  uint32_t v_val = c->swap_xy ? cx : cy;
  uint32_t v_span = c->swap_xy ? x_span : y_span;

  if (c->flip_x)
    h_val = h_span - h_val;
  if (c->flip_y)
    v_val = v_span - v_val;

  if (hid_x != 0) {
    *hid_x = (uint16_t)(((uint64_t)h_val * (TB371FC_HID_INPUT_X_MAX - 1u) +
                         h_span / 2u) / h_span);
  }
  if (hid_y != 0) {
    *hid_y = (uint16_t)(((uint64_t)v_val * (TB371FC_HID_INPUT_Y_MAX - 1u) +
                         v_span / 2u) / v_span);
  }
}

void
tb371fc_touch_to_hid(uint16_t native_x, uint16_t native_y,
                     uint16_t *hid_x, uint16_t *hid_y)
{
  tb371fc_touch_map_calibrated(native_x, native_y, 0, hid_x, hid_y);
}
