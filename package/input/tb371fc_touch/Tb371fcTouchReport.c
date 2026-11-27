/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb371fcTouchReport.h"

#define TB371FC_TOUCH_X_MAX 18400u
#define TB371FC_TOUCH_Y_MAX 29440u

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
tb371fc_touch_to_hid(uint16_t native_x, uint16_t native_y,
                     uint16_t *hid_x, uint16_t *hid_y)
{
  if (hid_x != 0) {
    if (native_y > TB371FC_TOUCH_Y_MAX)
      native_y = TB371FC_TOUCH_Y_MAX;
    *hid_x = (uint16_t)(((uint32_t)native_y *
                         (TB371FC_HID_INPUT_X_MAX - 1u) +
                         TB371FC_TOUCH_Y_MAX / 2u) / TB371FC_TOUCH_Y_MAX);
  }
  if (hid_y != 0) {
    if (native_x > TB371FC_TOUCH_X_MAX)
      native_x = TB371FC_TOUCH_X_MAX;
    *hid_y = (uint16_t)(((uint32_t)(TB371FC_TOUCH_X_MAX - native_x) *
                         (TB371FC_HID_INPUT_Y_MAX - 1u) + TB371FC_TOUCH_X_MAX / 2u) /
                        TB371FC_TOUCH_X_MAX);
  }
}
