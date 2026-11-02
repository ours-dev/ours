#!/usr/bin/env python3
"""One-off: split system/public/ours/syscalls.gni into per-category files.

Each entry moves verbatim into system/public/ours/syscalls/<category>.gni
(its handler file's name) and gains a `category` key. The top-level
syscalls.gni imports the category files and concatenates; the import order
becomes the call-number order, so every call is renumbered to sit inside its
category. Numbers are private to the kernel and the vDSO (design D8), and
both sides regenerate from this list, so programs see no change: the vDSO
exports the same ours_<name> symbols with the same signatures.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PUBLIC = ROOT / "system" / "public" / "ours"
SYSCALLS_GNI = PUBLIC / "syscalls.gni"

# category = the kernel/lib/syscall/src/<category>.cpp that defines the call.
CATEGORY = {
    "debug_write": "debug",
    "debug_read": "debug",
    "command_list": "command",
    "handle_close": "handle",
    "handle_duplicate": "handle",
    "channel_create": "channel",
    "channel_write": "channel",
    "channel_read": "channel",
    "object_wait_one": "object",
    "object_wait_async": "object",
    "object_get_info": "object",
    "object_get_child": "object",
    "object_set_property": "object",
    "port_create": "port",
    "port_queue": "port",
    "port_wait": "port",
    "process_exit": "process",
    "process_create": "process",
    "process_start": "process",
    "process_read_memory": "process",
    "thread_create": "task",
    "thread_start": "task",
    "thread_exit": "task",
    "task_suspend": "task",
    "task_kill": "task",
    "vmo_read": "vmo",
    "vmo_create": "vmo",
    "vmo_create_child": "vmo",
    "vmo_replace_as_executable": "vmo",
    "vmo_write": "vmo",
    "vmo_get_size": "vmo",
    "vmo_create_physical": "vmo",
    "vma_allocate": "vma",
    "vma_map": "vma",
    "vma_unmap": "vma",
    "task_create_exception_channel": "exception",
    "exception_get_thread": "exception",
    "exception_get_process": "exception",
    "interrupt_create": "interrupt",
    "interrupt_map": "interrupt",
    "interrupt_wait": "interrupt",
    "interrupt_trigger": "interrupt",
    "interrupt_destroy": "interrupt",
    "interrupt_bind": "interrupt",
    "interrupt_ack": "interrupt",
    "sampler_create": "sampler",
    "sampler_start": "sampler",
    "sampler_stop": "sampler",
    "sampler_read": "sampler",
    "ioports_request": "ioport",
    "ioports_release": "ioport",
    "pager_create": "pager",
    "pager_create_vmo": "pager",
    "pager_supply_pages": "pager",
    "pager_detach_vmo": "pager",
    "iommu_create": "bti",
    "bti_create": "bti",
    "bti_pin": "bti",
    "pmt_unpin": "bti",
    "bti_release_quarantine": "bti",
    "vmo_create_contiguous": "bti",
    "clock_get_monotonic": "time",
    "ticks_per_second": "time",
    "boot_offset": "time",
    "published_version": "time",
    "kernel_version": "time",
}

LICENSE = """\
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""

CATEGORY_BLURB = {
    "debug": "the debug console calls",
    "command": "the kernel command registry call",
    "handle": "the handle table calls",
    "channel": "the channel calls",
    "object": "the object calls (wait, info, child, property)",
    "port": "the port calls",
    "process": "the process calls",
    "task": "the thread and task calls (threads are made and started here; "
            "task.cpp holds them)",
    "vmo": "the VMO calls",
    "vma": "the address-region (Vma) calls",
    "exception": "the exception calls",
    "interrupt": "the interrupt calls",
    "sampler": "the sampling-profiler calls",
    "ioport": "the I/O port calls",
    "pager": "the pager calls",
    "bti": "the IOMMU, BTI, pin and contiguous-memory calls",
    "time": "the clock calls the vDSO answers directly",
}


def split_entries(text: str) -> list[str]:
    """The `  { ... },` blocks of ours_syscalls = [ ... ], verbatim."""
    start = text.index("ours_syscalls = [")
    body = text[start:]
    entries = []
    lines = body.split("\n")
    current: list[str] = []
    depth = 0
    for line in lines:
        if line == "  {":
            current = [line]
            depth = 1
        elif current:
            current.append(line)
            depth += line.count("{") - line.count("}")
            if depth == 0:
                entries.append("\n".join(current))
                current = []
    return entries


def entry_name(entry: str) -> str:
    return re.search(r'name = "(\w+)"', entry).group(1)


def main() -> None:
    text = SYSCALLS_GNI.read_text()
    entries = split_entries(text)
    names = [entry_name(e) for e in entries]
    unknown = [n for n in names if n not in CATEGORY]
    if unknown:
        sys.exit(f"no category for: {unknown}")

    # Split, preserving each entry's text and each category's internal order.
    per_category: dict[str, list[str]] = {}
    for entry in entries:
        name = entry_name(entry)
        cat = CATEGORY[name]
        lines = entry.rstrip().split("\n")
        # The captured block ends with the list-item comma after the scope's
        # closing brace: "  },". The category key goes inside the scope, as
        # its last key, after ret, so the ABI fields stay first.
        assert lines[-1] == "  },", f"unexpected entry end: {lines[-1]!r}"
        lines = lines[:-1] + [f'    category = "{cat}"', "  },"]
        per_category.setdefault(cat, []).append("\n".join(lines))

    order = []
    for name in names:
        cat = CATEGORY[name]
        if cat not in order:
            order.append(cat)

    out_dir = PUBLIC / "syscalls"
    out_dir.mkdir(exist_ok=True)
    for cat in order:
        body = "\n".join(per_category[cat])
        blurb = CATEGORY_BLURB[cat]
        out = (
            f"{LICENSE}\n"
            f"# {blurb[0].upper() + blurb[1:]}. One file per category; the\n"
            f"# import order in ../syscalls.gni, not this file, assigns the\n"
            f"# call numbers. Each entry names its category, which\n"
            f"# //build/abi/gen_syscalls.py uses to group the per-category\n"
            f"# declaration headers it emits. The calls' handlers live in\n"
            f"# //kernel/lib/syscall/src/{cat}.cpp.\n"
            f"ours_syscalls_{cat} = [\n{body}\n]\n"
        )
        (out_dir / f"{cat}.gni").write_text(out)
        print(f"wrote syscalls/{cat}.gni: {len(per_category[cat])} calls")

    # gn format sorts imports, so they are written sorted; the number order
    # is the concatenation below, which nothing but this file spells out.
    # An import is relative to the file that spells it, so from ours/ the
    # path drops the ours/ prefix. gn format sorts imports; the number order
    # is the concatenation below, which nothing but this file spells out.
    imports = "\n".join(
        f'import("syscalls/{cat}.gni")' for cat in sorted(order)
    )
    concat = " +\n    ".join(f"ours_syscalls_{cat}" for cat in order)
    header = re.search(r"(# Copyright.*?#\n)", text, re.S).group(1)
    top = (
        f"{header}\n"
        f"# The system calls, described once -- one file per category under\n"
        f"# ours/syscalls/, this file importing them in the order that\n"
        f"# assigns the call numbers.\n"
        f"#\n"
        f"# Everything that has to agree about a call is generated from the\n"
        f"# concatenated list by //build/abi/gen_syscalls.py: the private\n"
        f"# call numbers the kernel and the vDSO share, the vDSO stubs, the\n"
        f"# extern \"C\" declarations <ours/vdso.hpp> includes, the kernel's\n"
        f"# typed syscall::<name> declarations (no sys_ prefix) and the\n"
        f"# shims that unpack a register frame into them. GN derives\n"
        f"# libours.ifs, which the vDSO's exports are checked against, from\n"
        f"# the same list. Adding a call is two things by hand: an entry in\n"
        f"# ours/syscalls/<category>.gni, and the body of syscall::<name>()\n"
        f"# in //kernel/lib/syscall/src/<category>.cpp.\n"
        f"#\n"
        f"# The position of an entry is its call number. The numbers are\n"
        f"# private to the kernel and the vDSO -- a program names a call by\n"
        f"# its vDSO symbol, never by a number -- so the category order here\n"
        f"# renumbers them freely; the vDSO's symbols and signatures, the\n"
        f"# only ABI a program sees, do not move.\n"
        f"\n"
        f"{imports}\n"
        f"\n"
        f"ours_syscalls = {concat}\n"
    )
    SYSCALLS_GNI.write_text(top)
    print(f"calls: {len(entries)}; categories: {len(order)}")

    # The split must be faithful: same entries, none lost, none made up.
    assert len(entries) == sum(len(v) for v in per_category.values())
    assert sorted(names) == sorted(n for cat in order for n in
                                   (entry_name(e) for e in per_category[cat]))


if __name__ == "__main__":
    main()
