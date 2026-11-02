#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""The published-offset generator, on an ELF whose st_value is not a file offset.

argv: llvm-readelf clang ld.lld gen_published_table.py stamp
"""

import json
import os
import subprocess
import sys
import tempfile


def _run(argv):
    return subprocess.run(argv, check=False, capture_output=True, text=True)


def _die(message: str) -> None:
    print(f"gen_published_offsets_test: {message}", file=sys.stderr)
    sys.exit(1)


def _link(clang: str, linker: str, directory: str, where: str) -> str:
    assembly = os.path.join(directory, "record.S")
    script = os.path.join(directory, "record.ld")
    obj = os.path.join(directory, "record.o")
    elf = os.path.join(directory, "record.so")
    with open(assembly, "w", encoding="utf-8") as out:
        out.write(
            ".section .vdso_data, \"a\", @progbits\n"
            ".balign 8\n"
            ".global ours_time_values\n"
            ".hidden ours_time_values\n"
            ".type ours_time_values, @object\n"
            "ours_time_values:\n"
            ".quad 0\n"
            ".size ours_time_values, 8\n"
            ".section .text, \"ax\", @progbits\n"
            ".global _start\n"
            "_start:\n"
            "ret\n"
        )
        if where == "text":
            out.write(
                ".section .text, \"ax\", @progbits\n"
                ".global in_text\n"
                ".hidden in_text\n"
                "in_text:\n"
                "ret\n"
            )
    with open(script, "w", encoding="utf-8") as out:
        out.write(
            "PHDRS {\n"
            "  data PT_LOAD FLAGS(4);\n"
            "  text PT_LOAD FLAGS(5);\n"
            "}\n"
            "SECTIONS {\n"
            "  . = 0x2000;\n"
            "  .vdso_data : { *(.vdso_data) } :data\n"
            "  . = 0x4000;\n"
            "  .text : { *(.text) } :text\n"
            "}\n"
        )
    compiled = _run(
        [
            clang,
            "--target=x86_64-unknown-none-elf",
            "-c",
            assembly,
            "-o",
            obj,
        ]
    )
    if compiled.returncode != 0:
        _die(compiled.stderr)
    linked = _run(
        [linker, "-shared", "--no-rosegment", "-T", script, "-o", elf, obj]
    )
    if linked.returncode != 0:
        _die(linked.stderr)
    return elf


def _symbol(readelf: str, elf: str, name: str) -> tuple[int, int, int]:
    completed = _run(
        [readelf, "--elf-output-style=JSON", "--symbols", "--program-headers", elf]
    )
    if completed.returncode != 0:
        _die(completed.stderr)
    payload = json.loads(completed.stdout)
    value = None
    headers = []

    def walk(node) -> None:
        nonlocal value
        if isinstance(node, dict):
            symbol = node.get("Symbol")
            if isinstance(symbol, dict):
                symbol_name = symbol.get("Name")
                if isinstance(symbol_name, dict):
                    symbol_name = symbol_name.get("Name")
                if symbol_name == name:
                    value = int(symbol["Value"])
            program = node.get("ProgramHeader")
            if isinstance(program, dict) and program.get("Type", {}).get("Name") == "PT_LOAD":
                headers.append(program)
            for item in node.values():
                walk(item)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(payload)
    if value is None:
        _die(f"no {name}")
    file_offset = None
    image_base = None
    for header in headers:
        vaddr = int(header["VirtualAddress"])
        offset = int(header["Offset"])
        memsz = int(header["MemSize"])
        if image_base is None or vaddr < image_base:
            image_base = vaddr & ~0xFFF
        if vaddr <= value < vaddr + memsz:
            file_offset = offset + (value - vaddr)
    if file_offset is None or image_base is None:
        _die("symbol is not in a PT_LOAD")
    return value, image_base, file_offset


def main() -> None:
    if len(sys.argv) != 6:
        _die("usage: gen_published_offsets_test.py readelf clang ld.lld generator stamp")
    readelf, clang, linker, generator, stamp = sys.argv[1:]
    with tempfile.TemporaryDirectory() as directory:
        elf = _link(clang, linker, directory, "data")
        table = os.path.join(directory, "published")
        generated = _run(
            [sys.executable, generator, readelf, "section:.vdso_data", elf, table]
        )
        if generated.returncode != 0:
            _die(generated.stderr or "generator failed on a good record")
        text = open(table, encoding="utf-8").read().strip().split()
        if text[:1] != ["ours_time_values"] and "ours_time_values" not in text:
            _die(f"unexpected table {text}")
        # Last three tokens of the record line.
        record = [line for line in open(table, encoding="utf-8") if line.startswith("ours_time_values ")]
        if len(record) != 1:
            _die(f"expected one record, got {record}")
        _, vaddr_text, _size_text = record[0].split()
        vaddr = int(vaddr_text, 16)
        value, image_base, file_offset = _symbol(readelf, elf, "ours_time_values")
        if vaddr != value:
            _die(f"table vaddr {vaddr:#x} is not st_value {value:#x}")
        if (value - image_base) == file_offset:
            _die(
                "fixture does not separate st_value - image_base from file offset "
                f"({value:#x} base {image_base:#x} file {file_offset:#x})"
            )

        missing = _run([sys.executable, generator, readelf, "no_such", elf, table])
        if missing.returncode == 0:
            _die("a missing name was accepted")

        text_elf = _link(clang, linker, directory, "text")
        collected = _run(
            [sys.executable, generator, readelf, "section:.vdso_data", text_elf, table]
        )
        if collected.returncode != 0:
            _die(collected.stderr or "section collection failed")
        body = open(table, encoding="utf-8").read()
        if "in_text" in body:
            _die("a .text symbol was collected into .vdso_data")
        bare = os.path.join(directory, "bare.so")
        bare_s = os.path.join(directory, "bare.S")
        with open(bare_s, "w", encoding="utf-8") as out:
            out.write(".text\n.global _start\n_start:\nret\n")
        bare_o = os.path.join(directory, "bare.o")
        if _run([clang, "--target=x86_64-unknown-none-elf", "-c", bare_s, "-o", bare_o]).returncode != 0:
            _die("bare assemble failed")
        if _run([linker, "-shared", "-o", bare, bare_o]).returncode != 0:
            _die("bare link failed")
        empty = _run([sys.executable, generator, readelf, "section:.vdso_data", bare, table])
        if empty.returncode == 0:
            _die("an empty .vdso_data was accepted")

    with open(stamp, "w", encoding="utf-8") as out:
        out.write("ok\n")


if __name__ == "__main__":
    main()
