#!/usr/bin/env python3
"""Makes the raw GPT-partitioned disk image the block checks read.

The image carries what ours_gpt parses on a booted system: a protective MBR,
a GPT header at LBA 1, and the partition entry array at LBA 2, with one
partition of the "ours filesystem" type whose first block holds a marker the
booted check reads back. Usage:

    python3 scripts/make_gpt_disk.py <image> [<size-mib>]

The partition starts at LBA 2048, which keeps the write check's scratch LBA
1000 -- and anything a later milestone puts below the first partition --
outside the partition the marker lives in.
"""

import argparse
import uuid

LBA = 512
ENTRIES_LBA = 2
NUM_ENTRIES = 128
ENTRY_BYTES = 128
# The partition starts far enough past the table for anything to be added
# below it, and keeps the boot scripts' scratch LBA 1000 free.
PARTITION_FIRST_LBA = 2048
PARTITION_BLOCKS = 2048
# "ours filesystem", the type GUID ours_gpt names, in mixed-endian bytes.
OURS_FS_TYPE = uuid.UUID("5E3C4A9D-2B71-4F08-9A3D-6CE1F042B867").bytes_le
# What the booted check expects at the partition's first block.
MARKER = b"OURS-fs-partition\x00"


def crc32(data: bytes) -> int:
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF


def put16(raw: bytearray, offset: int, value: int) -> None:
    raw[offset : offset + 2] = value.to_bytes(2, "little")


def put32(raw: bytearray, offset: int, value: int) -> None:
    raw[offset : offset + 4] = value.to_bytes(4, "little")


def put64(raw: bytearray, offset: int, value: int) -> None:
    raw[offset : offset + 8] = value.to_bytes(8, "little")


def protective_mbr(first_usable_lba: int) -> bytearray:
    """LBA 0: one MBR entry of type 0xEE claiming the disk past its table."""
    raw = bytearray(LBA)
    # One partition entry: status 0, CHS addresses all a lie the legacy
    # readers accept, type 0xEE, the last sector of the disk.
    entry = bytearray(16)
    entry[4] = 0xEE
    put32(entry, 8, 1)
    put32(entry, 12, first_usable_lba - 1)
    raw[446:462] = entry
    raw[510:512] = b"\x55\xaa"
    return raw


def gpt_header(entries: bytearray, disk_blocks: int) -> bytearray:
    """LBA 1: the header, signed after the entry array's CRC goes in."""
    raw = bytearray(LBA)
    raw[0:8] = b"EFI PART"
    put32(raw, 8, 0x00010000)  # revision 1.0
    put32(raw, 12, 92)  # the header's own size, what its CRC covers
    put64(raw, 24, 1)  # MyLBA
    put64(raw, 32, disk_blocks - 1)  # AlternateLBA, the copy at the disk's end
    put64(raw, 40, PARTITION_FIRST_LBA)  # FirstUsableLBA
    # LastUsableLBA, before the backup table the alternate header sits over.
    put64(raw, 48, disk_blocks - 1 - 33)
    raw[56:72] = uuid.uuid4().bytes_le  # the disk's GUID, fresh each time
    put64(raw, 72, ENTRIES_LBA)
    put32(raw, 80, NUM_ENTRIES)
    put32(raw, 84, ENTRY_BYTES)
    put32(raw, 88, crc32(bytes(entries)))
    put32(raw, 16, crc32(bytes(raw[:92])))
    return raw


def entry_array() -> bytearray:
    """LBA 2 onwards: one used entry, the ours-fs partition, the rest free."""
    raw = bytearray(NUM_ENTRIES * ENTRY_BYTES)
    # A bytearray slice is a copy, not a view: the entry's fields are written
    # into |raw| itself, at entry 0's offsets.
    raw[0:16] = OURS_FS_TYPE
    raw[16:32] = uuid.uuid4().bytes_le  # the partition's own GUID
    put64(raw, 32, PARTITION_FIRST_LBA)
    put64(raw, 40, PARTITION_FIRST_LBA + PARTITION_BLOCKS - 1)
    name = "ours-fs".encode("utf-16-le")
    raw[56 : 56 + len(name)] = name
    return raw


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="the raw file to write")
    parser.add_argument("size_mib", nargs="?", type=int, default=64)
    args = parser.parse_args()

    disk_blocks = args.size_mib * 1024 * 1024 // LBA
    if disk_blocks < PARTITION_FIRST_LBA + PARTITION_BLOCKS:
        parser.error(f"{args.size_mib} MiB is too small for the partition this script writes")

    image = bytearray(disk_blocks * LBA)
    image[0:LBA] = protective_mbr(PARTITION_FIRST_LBA)
    # The array is made once and given to the header: a second call would
    # mint fresh GUIDs, and the CRC the header records would vouch for an
    # array that never lands on the disk.
    entries = entry_array()
    image[LBA : 2 * LBA] = gpt_header(entries, disk_blocks)
    image[2 * LBA : 2 * LBA + NUM_ENTRIES * ENTRY_BYTES] = entries
    # The partition's marker, at the partition's first block.
    start = PARTITION_FIRST_LBA * LBA
    image[start : start + len(MARKER)] = MARKER

    with open(args.image, "wb") as f:
        f.write(image)
    print(
        f"make_gpt_disk: {args.image}, {disk_blocks} blocks, "
        f"ours-fs at LBA {PARTITION_FIRST_LBA}-{PARTITION_FIRST_LBA + PARTITION_BLOCKS - 1}"
    )


if __name__ == "__main__":
    main()
