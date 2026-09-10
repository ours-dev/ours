#!/usr/bin/env python3
"""LLDB commands for reading ours kernel state that the debugger cannot see
on its own.

Load it once per session -- the VS Code launch configuration does this through
initCommands, so F5 needs no setup:

    command script import scripts/lldb_ours.py

Commands:

    cl <var> [cpu]    a CPU_LOCAL variable's type and value
    cl -a <var>       the same, on every CPU that has a unit
    cl -l             list the CPU_LOCAL variables the image defines

Why a command is needed at all
------------------------------
A CPU_LOCAL variable's symbol address is not where its value lives.  The symbol
names the *linker template* -- one copy, in .init.data -- and each CPU reads its
own copy at `template + offset`, where the offset is that CPU's distance from
the origin (kernel/mm/include/ours/mm/cpu_local.hpp).  So `p X86_PCPU` in lldb
prints the template: usually zeroes, and never the CPU you are looking at.

Two ways to get the offset, and this uses both:

  * the current CPU's is in %gs.  ArchCpuLocal::install() writes
    `base(cpu) - origin()` to IA32_GS_BASE, and every access is `%gs:&var`, so
    `gs_base + &var` is the address -- no target memory read needed, which
    matters because it keeps `cl` working before CpuLocal::init() has populated
    anything.
  * any other CPU's is CpuLocal::CPU_OFFSETS[cpu], the counterpart of Linux's
    __per_cpu_offset[].  Read out of target memory.

The addition wraps: the offsets are isize and negative in the physmap layout
(0xffffffffbff6e000 is -0x40092000), so it is done modulo 2**64 exactly as the
hardware does it.

The QEMU gdbstub presents each vCPU as a thread, so "the current CPU" is the
selected thread, and `cl <var>` follows a `thread select`.
"""

from __future__ import annotations

import argparse
import shlex

import lldb

MASK64 = (1 << 64) - 1

# CPU_OFFSETS is std::array<isize, MAX_CPU>; isize is 64-bit on every target
# this kernel builds for.
ENTRY_SIZE = 8

# CpuLocal's offset table -- Linux's __per_cpu_offset[]. Spelled out because
# nothing in the debug info marks it as special.
OFFSETS_SYMBOL = "ours::mm::CpuLocal::CPU_OFFSETS"

# The linker's own bounds on the per-CPU template block (kernel/kernel.ld:175,
# :204). This is the exact test for "is this variable CPU_LOCAL": the section
# name is not, because .init.data.cpu_local* is merged into .init.data, which
# also holds a great deal of ordinary init data.
TEMPLATE_BOUNDS = ("__cpu_local_start", "__cpu_local_end")


def _err(result: lldb.SBCommandReturnObject, message: str) -> None:
    result.SetError(f"cl: {message}")


def _find_global(target: lldb.SBTarget, name: str) -> lldb.SBValue | None:
    """The variable's template copy, by the name a human would type.

    Tried unqualified first: CPU_LOCAL variables are declared at namespace or
    file scope with names like X86_PCPU, and making people write
    ours::mm::CpuLocal::CURRENT_CPUNUM when CURRENT_CPUNUM is unambiguous is the
    kind of friction that stops a command being used.
    """
    for candidate in (name, f"ours::mm::{name}", f"ours::{name}"):
        found = target.FindGlobalVariables(candidate, 8)
        if found.GetSize() == 0:
            continue
        # An `inline` variable at namespace scope is described once per
        # translation unit that saw the header, so several matches are usually
        # one variable seen several times -- X86_PCPU comes back twice. They are
        # the same object exactly when they resolve to the same address; only
        # then is picking one of them safe.
        addresses = {found.GetValueAtIndex(n).GetLoadAddress()
                     for n in range(found.GetSize())}
        if len(addresses) == 1:
            return found.GetValueAtIndex(0)
        return None
    return None


def _offset_for_current(frame: lldb.SBFrame) -> tuple[int | None, str]:
    """This CPU's per-CPU offset, from whichever GS base currently holds it.

    Which one that is depends on the privilege level, and getting it wrong is
    silent: the kernel entry paths `swapgs`, so while the CPU is in ring 3 the
    per-CPU offset is parked in IA32_KERNEL_GS_BASE (the stub calls it
    k_gs_base) and %gs holds whatever userspace put there -- zero, for this
    kernel's userboot. Reading gs_base there yields 0 and every CPU_LOCAL
    variable resolves to its template.
    """
    cs = frame.FindRegister("cs")
    if not cs.IsValid():
        return None, "this target exposes no cs register"
    user_mode = (cs.GetValueAsUnsigned() & 3) == 3

    name = "k_gs_base" if user_mode else "gs_base"
    reg = frame.FindRegister(name)
    if not reg.IsValid():
        return None, (f"this target exposes no {name} register; "
                      f"name a CPU instead: cl <var> <cpu>")

    offset = reg.GetValueAsUnsigned() & MASK64
    if offset == 0:
        where = "in user mode" if user_mode else "in the kernel"
        return None, (f"{name} is 0 with the CPU {where} -- it has no per-CPU "
                      f"unit installed yet (before ArchCpuLocal::install)")
    return offset, ""


def _offsets_table(target: lldb.SBTarget) -> tuple[int, int, str]:
    """(load address, entry count) of CpuLocal::CPU_OFFSETS."""
    found = target.FindGlobalVariables(OFFSETS_SYMBOL, 1)
    if found.GetSize() != 1:
        return 0, 0, f"cannot find {OFFSETS_SYMBOL} in the image"
    table = found.GetValueAtIndex(0)
    address = table.GetLoadAddress()
    if address == lldb.LLDB_INVALID_ADDRESS:
        return 0, 0, f"{OFFSETS_SYMBOL} has no load address (is the image loaded?)"
    # Read it as bytes rather than through children: CPU_OFFSETS is a
    # std::array, whose children in lldb are whatever formatter happens to be
    # loaded -- in a freestanding build, usually the raw _M_elems member rather
    # than the elements. Its storage is a plain isize[] by definition, so
    # indexing the memory is both simpler and independent of that.
    return address, table.GetType().GetByteSize() // ENTRY_SIZE, ""


def _offset_for_cpu(target: lldb.SBTarget, cpu: int) -> tuple[int | None, str]:
    address, count, why = _offsets_table(target)
    if why:
        return None, why
    if not 0 <= cpu < count:
        return None, f"CPU {cpu} is outside {OFFSETS_SYMBOL}[{count}]"
    error = lldb.SBError()
    raw = target.GetProcess().ReadMemory(address + cpu * ENTRY_SIZE,
                                         ENTRY_SIZE, error)
    if error.Fail():
        return None, f"reading {OFFSETS_SYMBOL}[{cpu}]: {error.GetCString()}"
    value = int.from_bytes(raw, "little", signed=True)
    if value == 0:
        # Zero is not a valid unit offset once CpuLocal is up; it is what the
        # table holds for a CPU that never got a unit. Saying so beats printing
        # the template and letting it look like data.
        return None, (f"CPU {cpu} has no per-CPU unit "
                      f"({OFFSETS_SYMBOL}[{cpu}] is 0)")
    return value & MASK64, ""


def _template_bounds(target: lldb.SBTarget) -> tuple[int, int, str]:
    """[start, end) of the per-CPU template block, from the linker's symbols."""
    edges = []
    for name in TEMPLATE_BOUNDS:
        found = target.FindSymbols(name)
        if found.GetSize() < 1:
            return 0, 0, (f"the image defines no {name}; it is PROVIDE_HIDDEN in "
                          f"kernel/kernel.ld, so this is not an ours kernel")
        address = found.GetContextAtIndex(0).GetSymbol().GetStartAddress()
        edges.append(address.GetLoadAddress(target))
    if lldb.LLDB_INVALID_ADDRESS in edges:
        return 0, 0, "the template bounds have no load address (is the image loaded?)"
    return edges[0], edges[1], ""


def _in_template(target: lldb.SBTarget, value: lldb.SBValue) -> bool:
    start, end, why = _template_bounds(target)
    if why:
        return False
    where = value.GetLoadAddress()
    return where != lldb.LLDB_INVALID_ADDRESS and start <= where < end


def _render(target: lldb.SBTarget, value: lldb.SBValue, offset: int,
            label: str) -> str:
    template = value.GetLoadAddress()
    if template == lldb.LLDB_INVALID_ADDRESS:
        return f"{label}: the template has no load address (is the image loaded?)"

    resolved = (template + offset) & MASK64
    kind = value.GetType()
    here = target.CreateValueFromAddress(
        value.GetName(), lldb.SBAddress(resolved, target), kind)

    head = f"{label}  ({kind.GetName()}) {value.GetName()}"
    where = (f"        template {template:#x}  offset {offset:#x} "
             f"-> {resolved:#x}")

    scalar = here.GetValue()
    if scalar is not None:
        summary = here.GetSummary()
        return f"{head} = {scalar}{' ' + summary if summary else ''}\n{where}"

    # An aggregate has no scalar spelling, so the children are the answer. str()
    # of an SBValue already opens with "(type) name = ", which the head line
    # just said -- dropping that prefix keeps it from being printed twice.
    dump = str(here)
    marker = " = "
    if dump.startswith("(") and marker in dump.split("\n", 1)[0]:
        dump = dump.split(marker, 1)[1]
    return f"{head} = {dump}\n{where}"


def _list_cpu_locals(target: lldb.SBTarget, result: lldb.SBCommandReturnObject) -> None:
    start, end, why = _template_bounds(target)
    if why:
        _err(result, why)
        return
    names = set()
    for module in target.module_iter():
        for symbol in module:
            address = symbol.GetStartAddress().GetLoadAddress(target)
            if address == lldb.LLDB_INVALID_ADDRESS or not start <= address < end:
                continue
            name = symbol.GetName()
            # The bounds themselves, and the counter block's own marker, sit in
            # the range without being per-CPU variables.
            if name and not name.startswith("__"):
                names.add(name)
    if not names:
        _err(result, f"no symbols in [{start:#x}, {end:#x})")
        return
    result.AppendMessage(f"{len(names)} CPU_LOCAL variables in "
                         f"[{start:#x}, {end:#x}):\n" + "\n".join(sorted(names)))


def cl(debugger: lldb.SBDebugger, command: str, exe_ctx: lldb.SBExecutionContext,
       result: lldb.SBCommandReturnObject, _internal) -> None:
    parser = argparse.ArgumentParser(prog="cl", add_help=False)
    parser.add_argument("name", nargs="?")
    parser.add_argument("cpu", nargs="?", type=int)
    parser.add_argument("-a", "--all", action="store_true")
    parser.add_argument("-l", "--list", action="store_true")
    try:
        args = parser.parse_args(shlex.split(command))
    except SystemExit:
        _err(result, "usage: cl <var> [cpu] | cl -a <var> | cl -l")
        return

    target = exe_ctx.GetTarget()
    if not target.IsValid():
        _err(result, "no target")
        return

    if args.list:
        _list_cpu_locals(target, result)
        return

    if not args.name:
        _err(result, "usage: cl <var> [cpu] | cl -a <var> | cl -l")
        return

    value = _find_global(target, args.name)
    if value is None:
        _err(result, f"no single global variable named '{args.name}' "
                     f"(try the qualified name, or `cl -l`)")
        return

    # A plain global read through `cl` would silently apply an offset to
    # something that has no per-CPU copies, and print garbage that looks
    # plausible. Refuse instead, and say what to use.
    if not _in_template(target, value):
        _err(result, f"'{value.GetName()}' is not CPU_LOCAL "
                     f"(outside __cpu_local_start..__cpu_local_end) -- use `p`")
        return

    lines = []

    if args.all:
        _, count, why = _offsets_table(target)
        if why:
            _err(result, why)
            return
        for cpu in range(count):
            offset, why = _offset_for_cpu(target, cpu)
            if offset is None:
                continue          # CPUs without a unit are noise here, not news
            lines.append(_render(target, value, offset, f"cpu{cpu}"))
        if not lines:
            _err(result, "no CPU has a per-CPU unit yet")
            return
    elif args.cpu is not None:
        offset, why = _offset_for_cpu(target, args.cpu)
        if offset is None:
            _err(result, why)
            return
        lines.append(_render(target, value, offset, f"cpu{args.cpu}"))
    else:
        frame = exe_ctx.GetFrame()
        if not frame.IsValid():
            _err(result, "no frame; select a thread, or name a CPU: cl <var> <cpu>")
            return
        offset, why = _offset_for_current(frame)
        if offset is None:
            _err(result, why)
            return
        lines.append(_render(target, value, offset, "current"))

    result.AppendMessage("\n".join(lines))


def __lldb_init_module(debugger: lldb.SBDebugger, _internal) -> None:
    debugger.HandleCommand(
        f"command script add -o -f {__name__}.cl cl")
    print("ours: `cl <var> [cpu]` reads CPU_LOCAL variables (cl -l lists them)")
