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

Loading it also points the debug info's source paths, which are relative to the
build directory, at this checkout (see __lldb_init_module).

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
  * any other CPU's is CpuLocal::CPU_OFFSETS[cpu], the table of every CPU's
    offset.  Read out of target memory.

The addition wraps: the offsets are isize and negative in the physmap layout
(0xffffffffbff6e000 is -0x40092000), so it is done modulo 2**64 exactly as the
hardware does it.

The QEMU gdbstub presents each vCPU as a thread, so "the current CPU" is the
selected thread, and `cl <var>` follows a `thread select`.
"""

from __future__ import annotations

import argparse
import shlex
from pathlib import Path

import lldb

# The checkout this script belongs to, which is the one whose sources the
# debugger should show.
ROOT = Path(__file__).resolve().parent.parent

MASK64 = (1 << 64) - 1

# CPU_OFFSETS is std::array<isize, MAX_CPU>; isize is 64-bit on every target
# this kernel builds for.
ENTRY_SIZE = 8

# CpuLocal's offset table, one entry per CPU. Spelled out because nothing in
# the debug info marks it as special.
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
    # Objects record "." as their compilation directory and name every source
    # relative to the checkout root (//build/config:relative_paths):
    # kernel/top/panic.cpp. lldb tries such a path against its own working
    # directory first, so started in a checkout's root it reads that
    # checkout's files without help. Started anywhere else it finds nothing
    # there, and this mapping sends it to the checkout this script belongs
    # to. It is a setting of every target created after it, so it goes in
    # before the launch configuration's target create.
    debugger.HandleCommand(f'settings append target.source-map . "{ROOT}"')
    for name in ("cl", "cpus", "nodes", "kthread"):
        debugger.HandleCommand(f"command script add -o -f {__name__}.{name} {name}")
    print("ours: cl <var> [cpu] | cpus | nodes | kthread [cpu|0xaddr]")


# --------------------------------------------------------------------------
# Guarded objects.
#
# Most long-lived kernel objects open with a gktl::Canary seeded from their own
# type name (kernel/lib/gktl/include/gktl/canary.hpp).  The magic is a
# consteval hash of the fully-qualified name, so it can be recomputed here and
# used to answer "is this pointer really a Thread" -- which is the difference
# between a command that says "not a Thread" and one that prints noise from a
# stale pointer.
# --------------------------------------------------------------------------


def _magic(type_name: str) -> int:
    """gktl::magic(), reproduced. Must stay in step with canary.hpp."""
    def encode(c: str) -> int:
        if "A" <= c <= "Z":
            return ord(c) - ord("A")
        if "a" <= c <= "z":
            return ord(c) - ord("a") + 26
        if "0" <= c <= "9":
            return ord(c) - ord("0") + 52
        if c == "_":
            return 62
        return 63

    h = 0
    for char in type_name:
        h = (h * 63 + encode(char)) & 0xFFFFFFFF
    return h


def _canary(value: lldb.SBValue, type_name: str) -> str:
    """"" if the canary agrees, else why it does not.

    Zero and wrong are different answers and must not be merged. Canary's
    default constructor seeds zero (canary.hpp), so a zero magic means the
    object was never constructed -- a CPU that never came online, say -- while
    a non-zero mismatch means the bytes are somebody else's.
    """
    canary = value.GetChildMemberWithName("m_canary")
    if not canary.IsValid():
        return ""              # not a guarded type; nothing to disagree with
    magic = canary.GetChildMemberWithName("m_magic")
    if not magic.IsValid():
        return ""
    got = magic.GetValueAsUnsigned()
    if got == _magic(type_name):
        return ""
    return "unconstructed" if got == 0 else f"canary {got:#x}"


def _at(target: lldb.SBTarget, address: int, type_name: str) -> lldb.SBValue | None:
    kind = target.FindFirstType(type_name)
    if not kind.IsValid():
        return None
    return target.CreateValueFromAddress(
        type_name.rsplit("::", 1)[-1], lldb.SBAddress(address, target), kind)


def _percpu(target: lldb.SBTarget, symbol: str, offset: int) -> lldb.SBValue | None:
    """A CPU_LOCAL variable's copy for the CPU whose offset this is."""
    template = _find_global(target, symbol)
    if template is None:
        return None
    where = template.GetLoadAddress()
    if where == lldb.LLDB_INVALID_ADDRESS:
        return None
    return target.CreateValueFromAddress(
        template.GetName(), lldb.SBAddress((where + offset) & MASK64, target),
        template.GetType())


CURRENT_THREAD = "ours::mm::CpuLocal::Current<ours::task::Thread*>::CURRENT_INSTANCE"
CURRENT_SCHED = ("ours::mm::CpuLocal::Current<ours::task::sched::Scheduler>::"
                 "CURRENT_INSTANCE")


def _thread_summary(target: lldb.SBTarget, address: int) -> str:
    if address == 0:
        return "(none)"
    thread = _at(target, address, "ours::task::Thread")
    if thread is None:
        return f"{address:#x} (no ours::task::Thread type in the image)"
    bad = _canary(thread, "ours::task::Thread")
    if bad:
        return f"{address:#x} <{bad} -- not a Thread>"

    name = thread.GetChildMemberWithName("m_name").GetChildMemberWithName("name_")
    label = (name.GetSummary() or "?").strip('"') if name.IsValid() else "?"
    tid = thread.GetChildMemberWithName("m_tid")
    state = thread.GetChildMemberWithName("m_state")
    flags = thread.GetChildMemberWithName("m_flags")
    return (f"{label!r} tid={tid.GetValueAsUnsigned() if tid.IsValid() else '?'} "
            f"{state.GetValue() or '?'} [{flags.GetValue() or '?'}]")


def cpus(debugger: lldb.SBDebugger, command: str, exe_ctx: lldb.SBExecutionContext,
         result: lldb.SBCommandReturnObject, _internal) -> None:
    """What each CPU is running, from two independent places."""
    target = exe_ctx.GetTarget()
    if not target.IsValid():
        result.SetError("cpus: no target")
        return

    _, count, why = _offsets_table(target)
    if why:
        result.SetError(f"cpus: {why}")
        return

    # rqweight is the run queue's total, not any one thread's -- `kthread`
    # prints the per-thread weight, and two columns both called "weight" that
    # disagree is a trap.
    lines = [f"{'cpu':>3}  {'running':<44} {'queued':>6} {'rqweight':>9}  note"]
    seen = 0
    offline: list[int] = []
    for cpu in range(count):
        offset, _ = _offset_for_cpu(target, cpu)
        if offset is None:
            continue
        seen += 1

        pointer = _percpu(target, CURRENT_THREAD, offset)
        via_cpulocal = pointer.GetValueAsUnsigned() if pointer is not None else 0

        sched = _percpu(target, CURRENT_SCHED, offset)
        queued = weight = "?"
        via_sched = 0
        notes = []
        if sched is None:
            notes.append("no scheduler")
        else:
            bad = _canary(sched, "ours::task::sched::Scheduler")
            # A unit exists for every possible CPU, but only the onlined ones
            # ever construct a Scheduler in it. Listing the other twelve as
            # errors buries the four lines that matter, so they are counted and
            # summarised under the table instead.
            if bad == "unconstructed" and via_cpulocal == 0:
                offline.append(cpu)
                continue
            if bad:
                notes.append(f"scheduler {bad}")
            runtime = sched.GetChildMemberWithName("m_runtime")
            if runtime.IsValid():
                for field, into in (("num_queued_threads", "queued"),
                                    ("total_weight", "weight")):
                    child = runtime.GetChildMemberWithName(field)
                    if child.IsValid():
                        if into == "queued":
                            queued = str(child.GetValueAsUnsigned())
                        else:
                            weight = str(child.GetValueAsUnsigned())
                curr = runtime.GetChildMemberWithName("curr_thread")
                if curr.IsValid():
                    via_sched = curr.GetValueAsUnsigned()
                which = runtime.GetChildMemberWithName("which_cpu")
                # The scheduler records the CPU it belongs to. If that disagrees
                # with the offset we used to reach it, the per-CPU offset table
                # and the scheduler disagree about who this is -- worth more
                # than anything else on the line.
                if which.IsValid() and which.GetValueAsUnsigned() != cpu:
                    notes.append(f"which_cpu={which.GetValueAsUnsigned()}")

        # Two independent answers to "what is running": the per-CPU Thread*
        # and the scheduler's own curr_thread. They are updated separately, so
        # a mismatch is either a context switch caught in the middle or a bug,
        # and either way it is the thing to look at.
        if via_cpulocal and via_sched and via_cpulocal != via_sched:
            notes.append(f"sched says {via_sched:#x}")

        lines.append(f"{cpu:>3}  {_thread_summary(target, via_cpulocal):<44} "
                     f"{queued:>6} {weight:>9}  {', '.join(notes)}")

    if seen == 0:
        result.SetError("cpus: no CPU has a per-CPU unit yet")
        return
    if offline:
        lines.append(f"     ({len(offline)} more with a unit but no scheduler "
                     f"-- never onlined: "
                     f"{', '.join(str(c) for c in offline)})")
    result.AppendMessage("\n".join(lines))


def nodes(debugger: lldb.SBDebugger, command: str, exe_ctx: lldb.SBExecutionContext,
          result: lldb.SBCommandReturnObject, _internal) -> None:
    """Physical memory nodes and their zones."""
    target = exe_ctx.GetTarget()
    if not target.IsValid():
        result.SetError("nodes: no target")
        return

    table = _find_global(target, "ours::mm::PmNode::NODES")
    count_value = _find_global(target, "ours::mm::PmNode::NUM_NODES")
    if table is None or count_value is None:
        result.SetError("nodes: cannot find ours::mm::PmNode::NODES / NUM_NODES")
        return

    base = table.GetLoadAddress()
    slots = table.GetType().GetByteSize() // 8
    count = min(count_value.GetValueAsUnsigned(), slots)
    if count == 0:
        result.SetError("nodes: NUM_NODES is 0 -- mm has not come up yet")
        return

    process = target.GetProcess()
    page_kib = 4  # PAGE_SIZE; frames are counted in pages everywhere below

    lines = []
    for nid in range(count):
        error = lldb.SBError()
        raw = process.ReadMemory(base + nid * 8, 8, error)
        if error.Fail():
            lines.append(f"node {nid}: unreadable ({error.GetCString()})")
            continue
        pointer = int.from_bytes(raw, "little")
        if pointer == 0:
            lines.append(f"node {nid}: null")
            continue

        node = _at(target, pointer, "ours::mm::PmNode")
        if node is None:
            lines.append(f"node {nid}: {pointer:#x} (no ours::mm::PmNode type)")
            continue
        bad = _canary(node, "ours::mm::PmNode")
        if bad:
            lines.append(f"node {nid}: {pointer:#x} <{bad} -- not a PmNode>")
            continue

        def frames(name: str) -> int:
            child = node.GetChildMemberWithName(name)
            if not child.IsValid():
                return 0
            inner = child.GetChildMemberWithName("value_")
            return (inner if inner.IsValid() else child).GetValueAsUnsigned()

        spanned, present = frames("m_spanned_frames"), frames("m_present_frames")
        reserved = frames("m_reserved_frames")
        start = node.GetChildMemberWithName("m_start_pfn")
        lines.append(
            f"node {nid} @ {pointer:#x}  start_pfn="
            f"{start.GetValueAsUnsigned() if start.IsValid() else '?'}\n"
            f"    spanned  {spanned:>8} frames  {spanned * page_kib // 1024:>6} MiB\n"
            f"    present  {present:>8} frames  {present * page_kib // 1024:>6} MiB\n"
            f"    reserved {reserved:>8} frames  {reserved * page_kib // 1024:>6} MiB")

        # The zone queue is a fallback ordering, so the same zone appears more
        # than once; list each zone once, in the order first seen.
        queues = node.GetChildMemberWithName("m_zone_queues")
        storage = queues.GetChildMemberWithName("queue_storage_") if queues.IsValid() else queues
        if not storage.IsValid():
            continue
        zone_base = storage.GetLoadAddress()
        zone_slots = storage.GetType().GetByteSize() // 8
        shown = []
        for slot in range(zone_slots):
            raw = process.ReadMemory(zone_base + slot * 8, 8, error)
            if error.Fail():
                break
            zone_pointer = int.from_bytes(raw, "little")
            if zone_pointer == 0 or zone_pointer in shown:
                continue
            shown.append(zone_pointer)
            zone = _at(target, zone_pointer, "ours::mm::PmZone")
            if zone is None:
                lines.append(f"    zone @ {zone_pointer:#x} (no ours::mm::PmZone type)")
                continue

            def atomic(name: str) -> str:
                child = zone.GetChildMemberWithName(name)
                if not child.IsValid():
                    return "?"
                # std::atomic<usize> has no formatter in a freestanding build;
                # its single child is the stored value.
                inner = child.GetChildAtIndex(0)
                return str((inner if inner.IsValid() else child).GetValueAsUnsigned())

            kind = zone.GetChildMemberWithName("m_type")
            lines.append(f"    zone {kind.GetValue() or '?':<8} @ {zone_pointer:#x}  "
                         f"present={atomic('m_present_frames')} "
                         f"managed={atomic('m_managed_frames')}")

    result.AppendMessage("\n".join(lines))


# --------------------------------------------------------------------------
# One thread, in the detail that answers "why is this not running".
# --------------------------------------------------------------------------


def _child(value: lldb.SBValue, *path: str) -> lldb.SBValue:
    """Walk a member path, tolerating a missing link rather than raising."""
    for name in path:
        if not value.IsValid():
            return value
        value = value.GetChildMemberWithName(name)
    return value


def _u(value: lldb.SBValue, default: int = 0) -> int:
    return value.GetValueAsUnsigned() if value.IsValid() else default


def _enum(value: lldb.SBValue) -> str:
    return (value.GetValue() or "?") if value.IsValid() else "?"


def _lock_holder(lock: lldb.SBValue) -> str:
    """sync::SpinLock's word is 0 when free, else the holder's CPU number + 1.

    That encoding (kernel/include/ours/sync/spin_lock.hpp) is the whole reason
    this is worth printing: a chain of threads stuck behind each other names the
    CPU to go and look at, which no other field does.
    """
    word = _child(lock, "m_lockval", "value_")
    if not word.IsValid():
        word = _child(lock, "m_lockval")
    if not word.IsValid():
        return "?"
    raw = _u(word)
    return "free" if raw == 0 else f"held by cpu{raw - 1}"


def _thread_report(target: lldb.SBTarget, address: int, found_on: int | None) -> str:
    thread = _at(target, address, "ours::task::Thread")
    if thread is None:
        return f"{address:#x}: no ours::task::Thread type in the image"
    bad = _canary(thread, "ours::task::Thread")
    if bad:
        return f"{address:#x}: <{bad} -- not a Thread>"

    name = _child(thread, "m_name", "name_")
    label = (name.GetSummary() or "?").strip('"') if name.IsValid() else "?"
    state = _enum(_child(thread, "m_state"))

    context = _child(thread, "m_scheduling_context")
    waiter = _child(thread, "m_waiter_state")
    stack = _child(thread, "m_stack")

    curr_cpu = _u(_child(context, "m_curr_cpu"))
    weight = _u(_child(context, "m_profile", "effective", "weight"))
    queue = _child(waiter, "m_waitqueue")
    queue_at = _u(queue)

    base, size = _u(_child(stack, "m_base")), _u(_child(stack, "m_size"))
    sp = _u(_child(thread, "m_arch_thread", "m_sp"))

    out = [
        f"{label!r} tid={_u(_child(thread, 'm_tid'))} @ {address:#x}",
        f"  state    {state}   signals {_enum(_child(thread, 'm_signals'))}"
        f"   flags {_enum(_child(thread, 'm_flags'))}",
        f"  cpu      curr={curr_cpu} last={_u(_child(context, 'm_last_cpu'))}"
        f" wake={_u(_child(context, 'm_wake_cpu'))}",
        f"  sched    weight={weight}"
        f"  queue={_enum(_child(context, 'm_runqueue_state', 'm_queue_state'))}"
        f"/{_enum(_child(context, 'm_runqueue_state', 'm_transient_state'))}"
        f"  schedule={_u(_child(context, 'm_schedule')):#x}",
        f"  blocked  waitqueue={queue_at:#x}"
        f"  status={_enum(_child(waiter, 'm_blocked_status'))}"
        f"  interruptible={_enum(_child(waiter, 'm_interruptible'))}",
        f"  locks    resource={_lock_holder(_child(thread, 'm_resource_lock'))}"
        f"  pi={_lock_holder(_child(thread, 'm_pi_lock'))}",
        f"  stack    [{base:#x}, {base + size:#x}) {size} bytes  sp={sp:#x}",
        f"  aspace   {_u(_child(thread, 'm_aspace')):#x}"
        f"{'  (kernel thread)' if _u(_child(thread, 'm_aspace')) == 0 else ''}",
    ]

    # Only things that are actually wrong. The transient run-queue state is
    # deliberately NOT checked: kernel/include/ours/task/sched/context.hpp
    # records that Rescheduling is never cleared and DequeueDelayed is never
    # written, so a thread sitting in either is the known defect, not news --
    # and a command that cries wolf about it stops being read.
    alarms = []
    if state == "Parked" and queue_at == 0:
        alarms.append("Parked with no wait queue -- nothing will wake this")
    if weight == 0 and state in ("Running", "Ready"):
        alarms.append("effective weight is 0 -- avg_vruntime() divides by this")
    if found_on is not None and curr_cpu != found_on:
        # Distinguish "this thread is confused" from "nobody maintains these".
        # m_curr_cpu/m_last_cpu are written only by Scheduler::init_first_thread
        # and m_wake_cpu only by the Fair discipline, so on a tree running RR
        # they are all still zero -- that is a gap in the scheduler, not a fact
        # about this thread, and reporting it per thread would make the alarm
        # wallpaper. When it is fixed, the real check below starts working.
        if (curr_cpu, _u(_child(context, "m_last_cpu")),
                _u(_child(context, "m_wake_cpu"))) == (0, 0, 0):
            alarms.append("curr/last/wake_cpu all 0 -- nothing maintains them; "
                          "see docs/repairs/260910/task-sched.md")
        else:
            alarms.append(f"running on cpu{found_on} but curr_cpu says {curr_cpu}")
    if sp and size and not base <= sp <= base + size and state != "Running":
        alarms.append(f"sp {sp:#x} is outside its own kernel stack")
    for alarm in alarms:
        out.append(f"  !!       {alarm}")
    return "\n".join(out)


# Named kthread, not thread: lldb already has a `thread` command and will not
# let a script shadow it.
def kthread(debugger: lldb.SBDebugger, command: str, exe_ctx: lldb.SBExecutionContext,
           result: lldb.SBCommandReturnObject, _internal) -> None:
    """kthread            the thread on the selected CPU
       kthread <cpu>      the thread running on that CPU
       kthread 0x<addr>   that Thread, wherever it came from
    """
    target = exe_ctx.GetTarget()
    if not target.IsValid():
        result.SetError("kthread: no target")
        return

    argument = command.strip()

    # 0x means an address, anything else a CPU number. Distinguishing by
    # magnitude would be cleverer and would misread cpu 0x10 one day.
    if argument.startswith("0x"):
        try:
            result.AppendMessage(_thread_report(target, int(argument, 16), None))
        except ValueError:
            result.SetError(f"kthread: {argument} is not an address")
        return

    if argument:
        try:
            cpu = int(argument)
        except ValueError:
            result.SetError("kthread: usage: kthread [cpu | 0xaddress]")
            return
        offset, why = _offset_for_cpu(target, cpu)
        if offset is None:
            result.SetError(f"kthread: {why}")
            return
    else:
        frame = exe_ctx.GetFrame()
        if not frame.IsValid():
            result.SetError("kthread: no frame; name a CPU: kthread <cpu>")
            return
        offset, why = _offset_for_current(frame)
        if offset is None:
            result.SetError(f"kthread: {why}")
            return
        cpu = _u(_percpu(target, "ours::mm::CpuLocal::CURRENT_CPUNUM", offset))

    pointer = _percpu(target, CURRENT_THREAD, offset)
    if pointer is None or _u(pointer) == 0:
        result.SetError(f"kthread: cpu{cpu} has no current thread")
        return
    result.AppendMessage(_thread_report(target, _u(pointer), cpu))
