#!/usr/bin/env python3
"""Generate package/input/tb371fc_touch/Tb371fcTouchFirmware.c from stock ZUI 16.0.474 *_6.bin blobs.

Stock Lenovo TB371FC kernel driver (tp_compare_ic in boot.img) selects:
  - GPIO82 == 1 (Tianma, PID 0x6068): novatek_ts_tm_fw_6.bin (fw_ver=0x1a, 10x super-res 18400x29440)
  - GPIO82 == 0 (BOE,    PID 0x6067): novatek_ts_fw_boe_6.bin (fw_ver=0x18, 10x super-res 18400x29440)
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

EXPECTED_BYTES = 245760
EXPECTED_TM_SHA256_PREFIX = "9e8642c6c0d2f27a"
EXPECTED_BOE_SHA256_PREFIX = "6611efef1126dad4"


def format_c_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[] __attribute__((aligned(16))) = {{"]
    for off in range(0, len(data), 24):
        chunk = data[off : off + 24]
        lines.append(",".join(str(b) for b in chunk) + ",")
    lines.append("};")
    lines.append(f"const uint32_t {name}_size = {len(data)}u;")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--fw-dir",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "extracted" / "firmware",
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1]
        / "package"
        / "input"
        / "tb371fc_touch"
        / "Tb371fcTouchFirmware.c",
    )
    args = ap.parse_args()

    tm = (args.fw_dir / "novatek_ts_tm_fw_6.bin").read_bytes()
    boe = (args.fw_dir / "novatek_ts_fw_boe_6.bin").read_bytes()
    if len(tm) != EXPECTED_BYTES or len(boe) != EXPECTED_BYTES:
        raise ValueError(f"Unexpected firmware sizes: tm={len(tm)} boe={len(boe)}")

    tm_sha = hashlib.sha256(tm).hexdigest()
    boe_sha = hashlib.sha256(boe).hexdigest()
    if not tm_sha.startswith(EXPECTED_TM_SHA256_PREFIX):
        raise ValueError(f"Unexpected novatek_ts_tm_fw_6.bin sha256: {tm_sha}")
    if not boe_sha.startswith(EXPECTED_BOE_SHA256_PREFIX):
        raise ValueError(f"Unexpected novatek_ts_fw_boe_6.bin sha256: {boe_sha}")

    out = "\n".join(
        [
            "/* SPDX-License-Identifier: BSD-2-Clause-Patent */",
            "/* TB371FC Novatek NT36532 production 10x super-res firmware blobs (*_6.bin) from stock ZUI 16.0.474 */",
            f"/* Tianma novatek_ts_tm_fw_6.bin: fw_ver=0x{tm[0x352d0]:02x} sha256={tm_sha[:16]}... */",
            f"/* BOE    novatek_ts_fw_boe_6.bin: fw_ver=0x{boe[0x352d0]:02x} sha256={boe_sha[:16]}... */",
            "#include <stdint.h>",
            format_c_array("tb371fc_touch_fw_tm", tm),
            format_c_array("tb371fc_touch_fw_boe", boe),
            "",
        ]
    )
    args.output.write_text(out, encoding="utf-8", newline="\n")
    print(
        f"Wrote {args.output} ({len(out)} bytes): "
        f"tm=0x{tm[0x352d0]:02x} ({tm_sha[:16]}), boe=0x{boe[0x352d0]:02x} ({boe_sha[:16]})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
