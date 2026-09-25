/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Faults on command.
///
/// Started with one handle, a channel whose other end holds the name of a
/// probe. It reads the name, prints it, and does the one thing the probe
/// names -- each of them something a user program is not allowed to get
/// away with. The kernel's answer is to kill this process (P1-A); the peer
/// sees that as PeerClosed on its end of the channel with nothing to read.
///
/// If execution ever gets past the faulting instruction, the probe failed:
/// this writes "survived" back and spins, so that the driver can tell the
/// difference and say which probe it was. An unknown name is answered with
/// "unknown" the same way.
///
/// Every action is an `asm volatile`: a load whose result is unused, or a
/// division the compiler can see is by zero, is exactly what an optimiser
/// removes or folds, and the probe would then test nothing.

#include <ou/channel.hpp>
#include <ou/debug.hpp>
#include <ou/object.hpp>
#include <ou/str.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>

// trap.S: a `syscall` executed from this program's own text rather than the
// vDSO's.
extern "C" auto fault_probe_trap_outside_vdso() -> void;

namespace {
/// The address of the vDSO, for the probes that need a mapped page they may
/// not write to. Unused until such a probe is enabled (see PROBES).
using Action = auto (*)(ours::VirtAddr vdso_base) -> void;

struct Probe {
    ou::Str name;
    Action action;
};

/// A read of address 0x10: below USER_ASPACE_BASE, where nothing is ever
/// mapped. #PF, not present, user read.
auto pf_unmapped(ours::VirtAddr) -> void {
    u64 value = 0;
    __asm__ volatile("movq 0x10, %0" : "=r"(value));
}

/// A read of the first byte of the kernel half. #PF, present or not, user.
auto pf_kernel(ours::VirtAddr) -> void {
    u64 value = 0;
    __asm__ volatile("movabsq $0xffff800000000000, %%rax\n\t"
                     "movq (%%rax), %0"
                     : "=r"(value)
                     :
                     : "rax");
}

/// #UD.
auto ud2(ours::VirtAddr) -> void {
    __asm__ volatile("ud2");
}

/// A privileged instruction from ring 3: #GP(0).
auto gp_hlt(ours::VirtAddr) -> void {
    __asm__ volatile("hlt");
}

/// A port read. Every port is closed in the TSS I/O bitmap and ring 3 runs
/// at IOPL 0, so this is #GP(0) rather than a read of the POST port.
auto gp_io(ours::VirtAddr) -> void {
    u8 value = 0;
    __asm__ volatile("inb $0x80, %0" : "=a"(value));
}

/// A software interrupt through a gate ring 3 may not use: #GP, with the
/// error code naming the IDT entry.
auto gp_int80(ours::VirtAddr) -> void {
    __asm__ volatile("int $0x80");
}

/// The breakpoint instruction. Its IDT gate has DPL 0, so from ring 3 it is a
/// #GP naming entry 3 rather than a #BP.
auto gp_int3(ours::VirtAddr) -> void {
    __asm__ volatile("int3");
}

/// Division by zero: #DE.
auto de(ours::VirtAddr) -> void {
    __asm__ volatile("xorl %%ecx, %%ecx\n\t"
                     "divl %%ecx"
                     :
                     :
                     : "eax", "ecx", "edx", "cc");
}

/// The trap flag, set through popf -- which ring 3 may do -- so that the next
/// instruction ends in #DB.
auto db_tf(ours::VirtAddr) -> void {
    __asm__ volatile("pushfq\n\t"
                     "orq $0x100, (%%rsp)\n\t"
                     "popfq\n\t"
                     "nop"
                     :
                     :
                     : "memory", "cc");
}

/// A system call made without the vDSO: the kernel refuses the trap and
/// kills the process for it (a PolicyError, BadSyscall).
auto trap_outside_vdso(ours::VirtAddr) -> void {
    fault_probe_trap_outside_vdso();
}

/// Missing: a write to the vDSO (mapped read-only), which should end in a
/// #PF the address space refuses. Every x86 leaf page-table entry is built
/// writable today (docs/repairs/260924/x86-kernel.md, #1), so the write would
/// succeed and the probe would report "survived" for a reason that has
/// nothing to do with the fault path. It goes in with that fix.
constexpr Probe PROBES[] = {
    {"pf_unmapped", pf_unmapped},
    {"pf_kernel", pf_kernel},
    {"ud2", ud2},
    {"gp_hlt", gp_hlt},
    {"gp_int80", gp_int80},
    {"gp_int3", gp_int3},
    {"de", de},
    {"db_tf", db_tf},
    {"gp_io", gp_io},
    {"trap_outside_vdso", trap_outside_vdso},
};

auto same(ou::Str const a, ou::Str const b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (a.data()[i] != b.data()[i]) {
            return false;
        }
    }
    return true;
}

/// The end of a failed or unknown probe: say so to the driver, then stay.
/// There is no exit system call yet, and returning from _start would return
/// into whatever the kernel left on the stack.
[[noreturn]]
auto answer_and_stay(ours::HandleValue const channel, ou::Str const answer) -> void {
    static_cast<void>(ou::channel_write(channel, answer.data(), static_cast<u32>(answer.size())));
    for (;;) {
        __asm__ volatile("pause");
    }
}
} // namespace

/// |bootstrap| is the channel the driver holds the other end of (%rdi), and
/// |vdso_base| where the vDSO was mapped (%rsi).
extern "C" [[noreturn]]
auto _start(ours::HandleValue const bootstrap, ours::VirtAddr const vdso_base) -> void {
    using ours::Signals;

    auto observed = Signals::None;
    static_cast<void>(ou::object_wait_one(bootstrap, Signals::Readable | Signals::PeerClosed,
                                          ours::INSTANT_INFINITE, &observed));
    char buffer[32] = {};
    u32 actual = 0;
    if (ou::channel_read(bootstrap, buffer, sizeof(buffer), &actual) != ours::Status::Ok) {
        answer_and_stay(bootstrap, "no probe name");
    }
    auto const name = ou::Str(buffer, actual);
    static_cast<void>(ou::debug_line({"fault_probe: ", name}));

    for (auto const &probe: PROBES) {
        if (same(name, probe.name)) {
            probe.action(vdso_base);
            answer_and_stay(bootstrap, "survived");
        }
    }
    static_cast<void>(ou::debug_line({"fault_probe: unknown probe ", name}));
    answer_and_stay(bootstrap, "unknown");
}
