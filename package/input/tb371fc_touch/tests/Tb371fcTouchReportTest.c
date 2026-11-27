/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host sanitizer unit test for package/input/tb371fc_touch/Tb371fcTouchReport.c. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Tb371fcTouchReport.h"

static void finalize_checksum(uint8_t *report) {
  uint8_t sum = 0;
  unsigned i;
  for (i = 1; i < 65u; ++i)
    sum = (uint8_t)(sum + report[i]);
  report[65] = (uint8_t)(0u - sum);
}

int main(void) {
  uint8_t rpt[TB371FC_TOUCH_REPORT_BYTES];
  Tb371fcTouchFrame frame;
  uint16_t hx = 0, hy = 0;

  /* 1. Hardware-captured Tianma NT36532 12-bit touch frame (2944x1840):
   *    raw[1..6] = 0a 0b 3d 6b 02 01 -> id=1, state=2, x=182, y=987, w=2, p=1. */
  memset(rpt, 0, sizeof(rpt));
  rpt[1] = 0x0au;
  rpt[2] = 0x0bu;
  rpt[3] = 0x3du;
  rpt[4] = 0x6bu;
  rpt[5] = 0x02u;
  rpt[6] = 0x01u;
  finalize_checksum(rpt);
  assert(tb371fc_touch_checksum_ok(rpt, sizeof(rpt)) == 1);
  assert(tb371fc_touch_recovery(rpt, sizeof(rpt)) == 0);
  memset(&frame, 0, sizeof(frame));
  assert(tb371fc_touch_decode_ex(rpt, sizeof(rpt), 2944u, 1840u, &frame) == 0);
  assert(frame.count == 1u);
  assert(frame.contacts[0].id == 1u);
  assert(frame.contacts[0].x == 182u);
  assert(frame.contacts[0].y == 987u);
  assert(frame.contacts[0].width == 2u);
  assert(frame.contacts[0].pressure == 1u);

  /* 2. Tianma two-finger max-corner boundary (2944, 1840) + out-of-range rejection. */
  memset(rpt, 0, sizeof(rpt));
  /* Slot 0: id=1, state=1, x=2944 (0xB80), y=1840 (0x730), w=0->1, p=0->1 */
  rpt[1] = (1u << 3) | 1u;
  rpt[2] = 0xB8u;
  rpt[3] = 0x73u;
  rpt[4] = 0x00u;
  /* Slot 1: id=2, state=2, x=0, y=0, w=5, p=42 */
  rpt[7] = (2u << 3) | 2u;
  rpt[11] = 5u;
  rpt[12] = 42u;
  finalize_checksum(rpt);
  assert(tb371fc_touch_decode_ex(rpt, sizeof(rpt), 2944u, 1840u, &frame) == 0);
  assert(frame.count == 2u);
  assert(frame.contacts[0].x == 2944u && frame.contacts[0].y == 1840u);
  assert(frame.contacts[0].width == 1u && frame.contacts[0].pressure == 1u);
  assert(frame.contacts[1].id == 2u && frame.contacts[1].x == 0u && frame.contacts[1].y == 0u);
  assert(frame.contacts[1].width == 5u && frame.contacts[1].pressure == 42u);

  /* Out-of-bounds x=2945 (0xB81) must be rejected. */
  rpt[4] = 0x10u;
  finalize_checksum(rpt);
  assert(tb371fc_touch_decode_ex(rpt, sizeof(rpt), 2944u, 1840u, &frame) == -1);

  /* 3. High-res 16-bit big-endian report (18400x29440) via tb371fc_touch_decode. */
  memset(rpt, 0, sizeof(rpt));
  rpt[1] = (3u << 3) | 1u;
  rpt[2] = (uint8_t)(9200u >> 8);
  rpt[3] = (uint8_t)(9200u & 0xFFu);
  rpt[4] = (uint8_t)(14720u >> 8);
  rpt[5] = (uint8_t)(14720u & 0xFFu);
  rpt[6] = 9u;
  rpt[99] = 77u;
  finalize_checksum(rpt);
  assert(tb371fc_touch_decode(rpt, sizeof(rpt), &frame) == 0);
  assert(frame.count == 1u);
  assert(frame.contacts[0].id == 3u);
  assert(frame.contacts[0].x == 9200u && frame.contacts[0].y == 14720u);
  assert(frame.contacts[0].width == 9u && frame.contacts[0].pressure == 77u);

  /* 4. Checksum error + firmware handshake + watchdog recovery detection. */
  rpt[65] ^= 0xFFu;
  assert(tb371fc_touch_checksum_ok(rpt, sizeof(rpt)) == 0);
  assert(tb371fc_touch_decode(rpt, sizeof(rpt), &frame) == -1);

  memset(rpt, 0, sizeof(rpt));
  memset(rpt + 1, 0x77, 6);
  assert(tb371fc_touch_recovery(rpt, sizeof(rpt)) == 1);
  assert(tb371fc_touch_decode_ex(rpt, sizeof(rpt), 2944u, 1840u, &frame) == -2);

  memset(rpt + 1, 0xFE, 6);
  assert(tb371fc_touch_recovery(rpt, sizeof(rpt)) == 2);
  assert(tb371fc_touch_decode_ex(rpt, sizeof(rpt), 2944u, 1840u, &frame) == -2);

  /* 5. Portrait-to-landscape HID coordinate helper boundaries. */
  tb371fc_touch_to_hid(0u, 0u, &hx, &hy);
  assert(hx == 0u && hy == TB371FC_HID_INPUT_Y_MAX - 1u);
  tb371fc_touch_to_hid(18400u, 29440u, &hx, &hy);
  assert(hx == TB371FC_HID_INPUT_X_MAX - 1u && hy == 0u);

  printf("Tb371fcTouchReportTest: PASS\n");
  return 0;
}
