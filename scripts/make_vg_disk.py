#!/usr/bin/env python3
"""Makes the raw disk image the volume-manager round's on-target checks read.

The image carries a GPT with one partition of the "ours PV" type -- the
volume-manager's scan reads the device's first sectors directly and
skips what names no PV header, so the GUID is the disk saying what it is,
to humans and to other tools -- and the PV header at the partition's
first sectors, double written (LBA 0-1 primary, 2-3 secondary), with the
segment table the VG's shape asks for.

One image makes a linear VG of one PV (the boot_check's disk); two images
make a striped VG of two PVs, chunk 8 sectors -- the demo's cross-disk
shape, DISK + NVME_DISK. The LV is named "root" and spans both extents
of each PV. Usage:

    python3 scripts/make_vg_disk.py <image> [<image2>]

The two-image form takes the same size on both (64 MiB each, as the GPT
maker's output).
"""

from __future__ import annotations

import sys
import uuid
import zlib
from pathlib import Path

LBA = 512
# The partition starts far enough past the table for anything to be added
# below it, as the GPT maker's ours-fs partition does.
PARTITION_FIRST_LBA = 2048
# The partition's size in LBAs, 64 MiB minus the table's own space, the
# GPT maker's default shape.
PARTITION_BLOCKS = 131072 - 2048
# "ours PV", the type GUID the volume round names, in mixed-endian bytes.
OURS_PV_TYPE = uuid.UUID("7E1C4A9D-2B71-4F08-9A3D-6CE1F042B868").bytes_le
# One extent: 1 MiB, the mapping's unit, in sectors.
EXTENT_SECTORS = 2048
# The metadata block: HEADER_SECTORS sectors, written twice.
HEADER_BYTES = 1024
HEADER_SECTORS = 2
# The striped LV's chunk, in sectors: 8, small enough that the round's
# checks walk the round-robin by hand.
CHUNK_SECTORS = 8


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def put16(raw: bytearray, offset: int, value: int) -> None:
    raw[offset : offset + 2] = value.to_bytes(2, "little")


def put32(raw: bytearray, offset: int, value: int) -> None:
    raw[offset : offset + 4] = value.to_bytes(4, "little")


def put64(raw: bytearray, offset: int, value: int) -> None:
    raw[offset : offset + 8] = value.to_bytes(8, "little")


def protective_mbr(first_usable_lba: int) -> bytearray:
    raw = bytearray(LBA)
    entry = bytearray(16)
    entry[4] = 0xEE
    put32(entry, 8, 1)
    put32(entry, 12, first_usable_lba - 1)
    raw[446:462] = entry
    put16(raw, 510, 0xA5)
    return raw


def gpt_header(crc_of_partition_entries: int, first_usable_lba: int, disk_blocks: int) -> bytearray:
    raw = bytearray(LBA)
    raw[0:8] = b"EFI PART"
    put32(raw, 8, 0x00010000)
    put32(raw, 12, LBA)
    put64(raw, 24, 1)
    put64(raw, 32, crc32(bytes(raw[0:92])))
    put32(raw, 48, 0)
    put64(raw, 72, 2)
    put64(raw, 80, 33)
    put64(raw, 88, disk_blocks - 1)
    put64(raw, 96, disk_blocks)
    raw[104:120] = uuid.uuid4().bytes
    put64(raw, 120, 2)
    put32(raw, 128, 128)
    put32(raw, 132, 128)
    put32(raw, 136, crc_of_partition_entries)
    raw[144:144 + LBA - 144] = b"\0" * (LBA - 144)
    return raw


def partition_entry(type_guid: bytes, first: int, last: int, name: str, index: int) -> bytearray:
    raw = bytearray(128)
    raw[0:16] = type_guid
    raw[16:32] = uuid.uuid4().bytes_le
    put64(raw, 32, first)
    put64(raw, 40, last)
    put64(raw, 48, 0)
    put64(raw, 56, 0)
    named = name.encode("UTF-16-LE")[:72]
    raw[56 * 2 : 56 * 2 + len(named)] = named
    return raw


def gpt_entries(entries: list[bytearray]) -> bytearray:
    raw = bytearray(128 * 128)
    for index, entry in enumerate(entries):
        raw[index * 128 : (index + 1) * 128] = entry
    put32(raw, 0, 0)  # placeholder; the CRC is of the whole array
    return raw


def pv_header_bytes(pv_index: int, pv_uuid: bytes, lv_count: int, sector_count: int,
                    segments: list[tuple]) -> bytes:
    """The PV header, the wire form system/lib/volume's codec reads: the
    fields little-endian, the LV slots and segments at their offsets, the
    CRC over the whole block with its own field zero."""
    raw = bytearray(HEADER_BYTES)
    raw[0:8] = b"OURSPV\0\0"
    put32(raw, 8, 1)
    raw[12:28] = pv_uuid
    # The VG's uuid is the group's: shared by every PV, or the assembler
    # reads each disk as its own group and refuses the second.
    raw[28:44] = uuid.UUID("0F14E7A2-88C1-4B6D-9A3E-52C7D1B04A19").bytes
    raw[44:44 + 3] = b"vg0"
    put32(raw, 60, EXTENT_SECTORS)
    put32(raw, 64, pv_index)
    put32(raw, 68, 2)  # this PV holds extents 0-1
    put32(raw, 72, lv_count)
    put32(raw, 76, len(segments))

    def put_lv(slot: int, name: bytes, kind: int, chunk: int, stripes: int, sectors: int) -> None:
        at = 80 + slot * 40
        raw[at : at + len(name)] = name
        put32(raw, at + 16, kind)
        put32(raw, at + 20, chunk)
        put32(raw, at + 24, stripes)
        put64(raw, at + 28, sectors)

    if lv_count == 1:
        put_lv(0, b"root", 0, 0, 0, sector_count)
    else:
        put_lv(0, b"root", 0, 0, 0, sector_count)
        put_lv(1, b"st", 1, CHUNK_SECTORS, 2, sector_count)

    for index, segment in enumerate(segments):
        lv, pv, role, logical, physical, count = segment
        at = 80 + 4 * 40 + index * 20
        put32(raw, at, lv | (pv << 16))
        put32(raw, at + 4, role)
        put32(raw, at + 8, logical)
        put32(raw, at + 12, physical)
        put32(raw, at + 16, count)

    # The CRC over the whole block with its own field zero -- the field
    # is still zero here -- which is the decode's form; the shorter
    # range's CRC is a different number and the decode refuses it.
    put32(raw, HEADER_BYTES - 4, crc32(bytes(raw)))
    return bytes(raw)


def make_pv_image(path: Path, pv_index: int, two_legged: bool) -> None:
    """Writes |path|: the GPT with one ours-PV partition, and the PV
    header's two copies in the partition's first sectors."""
    total_blocks = PARTITION_FIRST_LBA + PARTITION_BLOCKS
    image = bytearray(total_blocks * LBA)

    # The LV's shape: one linear LV spanning both PVs' extents, and -- for
    # the two-legged VG -- a striped one over the same extents.
    extent_sectors = EXTENT_SECTORS
    if two_legged:
        lv_count, sector_count = 2, 2 * extent_sectors
        # PV |pv_index| carries the linear piece at logical extent
        # |pv_index| (physical extent 0), and the stripe leg |pv_index|
        # (physical extent 1).
        segments = [
            (0, pv_index, 0, pv_index, 0, 1),
            (1, pv_index, 1, pv_index, 1, 1),
        ]
    else:
        lv_count, sector_count = 1, 2 * extent_sectors
        segments = [(0, pv_index, 0, pv_index * 2, 0, 2)]

    header = pv_header_bytes(pv_index, uuid.uuid4().bytes, lv_count, sector_count, segments)
    # The header rides the device's own first sectors: the volume-manager
    # routes whole devices and reads them at LBA 0 -- the GPT is the
    # disk's self-description, and the scan never walks it.
    image[0:HEADER_BYTES] = header
    image[HEADER_SECTORS * LBA : HEADER_SECTORS * LBA + HEADER_BYTES] = header

    # No GPT: the volume-manager routes whole devices and reads the
    # device's first sectors -- the PV header at LBA 0-3 is the disk's
    # whole self-description, and a table nobody reads is dead weight the
    # header's own extents would collide with.
    path.write_bytes(image)
    print(f"make_vg_disk: {path}, PV {pv_index}, "
          f"{'striped of two' if two_legged else 'linear'}, "
          f"LV extents at LBAs {PARTITION_FIRST_LBA * EXTENT_SECTORS // EXTENT_SECTORS}+")


def main(argv: list[str]) -> int:
    if len(argv) not in (1, 2):
        print(__doc__, file=sys.stderr)
        return 2
    make_pv_image(Path(argv[0]), 0, two_legged=len(argv) == 2)
    if len(argv) == 2:
        make_pv_image(Path(argv[1]), 1, two_legged=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
