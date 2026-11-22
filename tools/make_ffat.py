#!/usr/bin/env python3
"""Build a FAT16 image of us1.rom for the ffat partition at 0x610000.

The firmware reads the ROM bytes raw at offset 0x9200. It does not mount
this image through the Arduino FFat library. Usage:

    python3 tools/make_ffat.py /path/to/us1.rom

Writes ffat.img in the current directory.
"""

from __future__ import annotations

import sys
from pathlib import Path

# app3M_fat9M_16MB: ffat at 0x610000, size 0x9E0000.
PARTITION = 0x9E0000
SECTOR = 512
# 2048-byte clusters put the volume above FatFs's FAT12 cutoff (4085).
SPC = 4
RESERVED = 1
FATS = 2
ROOT_ENTRIES = 512
# Locked to the us1 image the firmware copies into PSRAM.
ROM_BYTES = 7282628
ROM_AT = 0x9200


def fat_layout(total: int) -> tuple[int, int]:
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    fat_secs = 1
    clusters = 0
    for _ in range(8):
        data = total - RESERVED - FATS * fat_secs - root_sectors
        clusters = data // SPC
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need == fat_secs:
            return fat_secs, clusters
        fat_secs = need
    raise SystemExit("FAT size did not converge")


def put16(buf: bytearray, at: int, value: int) -> None:
    buf[at:at + 2] = value.to_bytes(2, "little")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: python3 tools/make_ffat.py /path/to/us1.rom")
    rom_path = Path(sys.argv[1])
    rom = rom_path.read_bytes()
    if len(rom) != ROM_BYTES:
        raise SystemExit(f"{rom_path} is {len(rom)} bytes, us1 is {ROM_BYTES}")
    if rom[0x30:0x34] != b"MBRO":
        raise SystemExit(f"{rom_path} has no MBRO header at 0x30")

    total = PARTITION // SECTOR
    fat_secs, clusters = fat_layout(total)
    if not (4085 < clusters < 65525):
        raise SystemExit(f"cluster count {clusters} is not FAT16")
    cluster_bytes = SPC * SECTOR
    used = (len(rom) + cluster_bytes - 1) // cluster_bytes
    if used > clusters:
        raise SystemExit(f"rom needs {used} clusters, image has {clusters}")

    image = bytearray(PARTITION)
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"MSDOS5.0"
    put16(boot, 11, SECTOR)
    boot[13] = SPC
    put16(boot, 14, RESERVED)
    boot[16] = FATS
    put16(boot, 17, ROOT_ENTRIES)
    put16(boot, 19, total)
    boot[21] = 0xF8
    put16(boot, 22, fat_secs)
    put16(boot, 24, 32)
    put16(boot, 26, 64)
    boot[36] = 0x80
    boot[38] = 0x29
    boot[39:43] = b"\x21\x11\x20\x26"
    boot[43:54] = b"VOICE      "
    boot[54:62] = b"FAT16   "
    boot[510] = 0x55
    boot[511] = 0xAA
    image[0:SECTOR] = boot

    fat = bytearray(fat_secs * SECTOR)
    put16(fat, 0, 0xFFF8)
    put16(fat, 2, 0xFFFF)
    for i in range(used - 1):
        put16(fat, (2 + i) * 2, 3 + i)
    put16(fat, (2 + used - 1) * 2, 0xFFFF)
    fat_at = RESERVED * SECTOR
    image[fat_at:fat_at + len(fat)] = fat
    image[fat_at + len(fat):fat_at + 2 * len(fat)] = fat

    # 2026-11-21, the day this image format was locked.
    date = ((2026 - 1980) << 9) | (11 << 5) | 21
    short = b"US1     ROM"
    checksum = 0
    for byte in short:
        checksum = ((checksum & 1) << 7) + (checksum >> 1) + byte
        checksum &= 0xFF
    # One long-name slot so a FAT reader opens "/us1.rom" either way.
    lfn = bytearray(32)
    lfn[0] = 0x41
    chars = "us1.rom"
    slots = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
    encoded = [ord(ch) for ch in chars] + [0x0000]
    encoded += [0xFFFF] * (13 - len(encoded))
    for slot, ch in zip(slots, encoded):
        put16(lfn, slot, ch)
    lfn[11] = 0x0F
    lfn[13] = checksum
    root = bytearray(32)
    root[0:11] = short
    root[11] = 0x20
    put16(root, 14, 0)
    put16(root, 16, date)
    put16(root, 18, date)
    put16(root, 26, 2)
    root[28:32] = len(rom).to_bytes(4, "little")
    root_at = (RESERVED + FATS * fat_secs) * SECTOR
    image[root_at:root_at + 32] = lfn
    image[root_at + 32:root_at + 64] = root

    data_at = root_at + ROOT_ENTRIES * 32
    if data_at != ROM_AT:
        raise SystemExit(f"ROM landed at {data_at:#x}, firmware reads {ROM_AT:#x}")
    image[data_at:data_at + len(rom)] = rom

    got = bytearray()
    cluster = 2
    while cluster < 0xFFF8:
        off = data_at + (cluster - 2) * cluster_bytes
        got += image[off:off + cluster_bytes]
        cluster = int.from_bytes(fat[cluster * 2:cluster * 2 + 2], "little")
    if bytes(got[:len(rom)]) != rom:
        raise SystemExit("FAT readback does not match us1.rom")

    image_path = Path.cwd() / "ffat.img"
    image_path.write_bytes(image)
    print(f"ffat {image_path} {len(image)} bytes, us1.rom {len(rom)} bytes")
    print(f"esptool.py --chip esp32s3 write_flash 0x610000 {image_path}")


if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError as exc:
        sys.exit(f"missing {exc.filename}")
