#!/usr/bin/env python3
"""Validate the Multiboot2 request header emitted by the real physboot ELF."""

from __future__ import annotations

import argparse
import pathlib
import struct


MULTIBOOT2_HEADER_MAGIC = 0xE85250D6
MULTIBOOT_HEADER_TAG_END = 0
MULTIBOOT_HEADER_TAG_INFORMATION_REQUEST = 1
MULTIBOOT_TAG_TYPE_MODULE = 3
MULTIBOOT_TAG_TYPE_MMAP = 6


def boot_header_section(image: bytes) -> tuple[int, bytes]:
    if image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        raise ValueError("physboot is not a little-endian ELF64 image")
    elf = struct.Struct("<16sHHIQQQIHHHHHH")
    (_, _, _, _, _, _, section_offset, _, _, _, _, section_size, section_count,
     string_section_index) = elf.unpack_from(image)
    section = struct.Struct("<IIQQQQIIQQ")
    if section_size != section.size or string_section_index >= section_count:
        raise ValueError("physboot ELF section table is malformed")
    section_table_size = section_size * section_count
    if section_offset + section_table_size > len(image):
        raise ValueError("physboot ELF section table is out of bounds")

    string_header = section.unpack_from(image, section_offset + string_section_index * section_size)
    string_offset, string_size = string_header[4:6]
    if string_offset + string_size > len(image):
        raise ValueError("physboot ELF section names are out of bounds")
    names = image[string_offset:string_offset + string_size]

    for index in range(section_count):
        header = section.unpack_from(image, section_offset + index * section_size)
        name_offset, _, _, _, offset, size, _, _, _, _ = header
        if name_offset >= len(names) or offset + size > len(image):
            raise ValueError("physboot ELF section is out of bounds")
        name_end = names.find(b"\0", name_offset)
        if name_end < 0:
            raise ValueError("physboot ELF section name is unterminated")
        if names[name_offset:name_end] == b".boot":
            return offset, image[offset:offset + size]
    raise ValueError("physboot ELF has no .boot section")


def validate(image: bytes) -> None:
    section_offset, data = boot_header_section(image)
    header_offset = None
    for offset in range(0, max(0, len(data) - 3), 8):
        if struct.unpack_from("<I", data, offset)[0] == MULTIBOOT2_HEADER_MAGIC:
            header_offset = offset
            break
    if header_offset is None or (section_offset + header_offset) % 8 != 0:
        raise ValueError("Multiboot2 header is not 8-byte aligned")
    magic, _, length, checksum = struct.unpack_from("<IIII", data, header_offset)
    if magic != MULTIBOOT2_HEADER_MAGIC or (magic + length + checksum) & 0xFFFFFFFF:
        raise ValueError("Multiboot2 fixed header is malformed")
    if length % 8 != 0 or header_offset + length > len(data):
        raise ValueError("Multiboot2 header has an invalid length")

    requested: set[int] = set()
    cursor = header_offset + 16
    end = header_offset + length
    while cursor < end:
        tag_type, flags, tag_size = struct.unpack_from("<HHI", data, cursor)
        if tag_size < 8 or cursor + tag_size > end:
            raise ValueError("Multiboot2 header tag is malformed")
        if tag_type == MULTIBOOT_HEADER_TAG_INFORMATION_REQUEST:
            if flags != 0 or (tag_size - 8) % 4:
                raise ValueError("Multiboot2 information request is malformed")
            requested.update(struct.unpack_from(f"<{(tag_size - 8) // 4}I", data, cursor + 8))
        if tag_type == MULTIBOOT_HEADER_TAG_END:
            if tag_size != 8 or cursor + tag_size != end:
                raise ValueError("Multiboot2 end tag is malformed")
            break
        cursor = (cursor + tag_size + 7) & ~7
    else:
        raise ValueError("Multiboot2 header has no end tag")
    if not {MULTIBOOT_TAG_TYPE_MODULE, MULTIBOOT_TAG_TYPE_MMAP}.issubset(requested):
        raise ValueError("Multiboot2 header does not require OBI module and memory map tags")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=pathlib.Path, required=True)
    parser.add_argument("--stamp", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    validate(arguments.elf.read_bytes())
    arguments.stamp.parent.mkdir(parents=True, exist_ok=True)
    arguments.stamp.write_text("ok\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
