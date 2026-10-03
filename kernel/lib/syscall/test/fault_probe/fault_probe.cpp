/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Faults on command.
///
/// Started on the runtime (//system/lib/ort): the probe's name is
/// argv[1], and the one startup handle is a channel (User0) whose other end
/// the driver holds. It prints the name and does the one thing the probe
/// names -- each of them something a user program is not allowed to get
/// away with. The kernel's answer is to kill this process (P1-A); the driver
/// sees that as PeerClosed on its end of the channel with nothing to read.
///
/// If execution ever gets past the faulting instruction, the probe failed:
/// this writes "survived" back and returns from main, so that the driver can
/// tell the difference and say which probe it was. An unknown name is
/// answered with "unknown" the same way. Returning is what makes the second
/// case worth having: the driver runs one deliberately unknown probe, and
/// that is the only run in which main returns and the runtime ends the
/// process with main's value.
///
/// Every action is an `asm volatile`: a load whose result is unused, or a
/// division the compiler can see is by zero, is exactly what an optimiser
/// removes or folds, and the probe would then test nothing.
///
/// Before any of that, it checks that a table of pointers in its writable
/// data was relocated where it lies (see RELOCATED), and answers "relocation"
/// if not: the check on the loader that started it, whichever that was.

#include <algorithm>
#include <string_view>

#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours/process_args.hpp>
#include <ours/vdso.hpp>

// trap.S: a `syscall` executed from this program's own text rather than the
// vDSO's.
extern "C" auto fault_probe_trap_outside_vdso() -> void;

namespace {
/// What a probe does. |vdso_base| is for the one that needs a page that is
/// mapped but may not be written.
using Action = auto (*)(ours::VirtAddr vdso_base) -> void;

struct Probe {
    std::string_view name;
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

/// A write to the vDSO, which every process has mapped read-only. #PF,
/// present, user write -- a protection violation the address space refuses,
/// where pf_unmapped is a page that is not there at all. Until leaf page-table
/// entries stopped being built writable whatever they asked for, this write
/// went through and the probe reported "survived".
auto pf_write_vdso(ours::VirtAddr const vdso_base) -> void {
    __asm__ volatile("movb $0, (%0)" : : "r"(vdso_base) : "memory");
}

/// A call into the vDSO's first page: its ELF header and symbol tables, data
/// mapped read-only and not executable since the vDSO is mapped by segment.
/// #PF, present, user instruction fetch. While the vDSO was mapped whole and
/// executable, this ran the header's bytes as code, inside the one range the
/// kernel accepts a `syscall` from.
auto pf_exec_vdso_data(ours::VirtAddr const vdso_base) -> void {
    auto const data = reinterpret_cast<void (*)()>(vdso_base);
    data();
}

/// A store into this program's own code: the first byte of this function,
/// written back with the byte it holds, so that a store that goes through
/// changes nothing and the probe returns to say so. #PF, present, user
/// write. While the kernel mapped a program as one range, read, write and
/// execute, it went through.
auto pf_write_text(ours::VirtAddr) -> void {
    auto const code = reinterpret_cast<ours::VirtAddr>(&pf_write_text);
    __asm__ volatile("movb (%0), %%al\n\t"
                     "movb %%al, (%0)"
                     :
                     : "r"(code)
                     : "rax", "memory");
}

/// A `ret`, in writable data: not const, so that the linker puts it in .data,
/// and its address handed to an asm (pf_exec_data()), so that the compiler
/// cannot prove nothing writes it and move it to .rodata itself.
u8 ret_in_data[] = {0xC3};

/// A call into |ret_in_data|, whose page is the program's data, mapped read
/// and write and not executable. #PF, present, user instruction fetch. While
/// the kernel mapped a program as one range, read, write and execute, the
/// `ret` ran and the probe returned.
auto pf_exec_data(ours::VirtAddr) -> void {
    auto code = reinterpret_cast<ours::VirtAddr>(ret_in_data);
    // From here the compiler knows nothing of where |code| points: what is
    // called is whatever is there, not a call through a pointer to an object,
    // which it could take for undefined behaviour and drop.
    __asm__ volatile("" : "+r"(code) : : "memory");
    reinterpret_cast<void (*)()>(code)();
}

/// Pointers to string literals, constant. In a PIE each is a relocation
/// (R_X86_64_RELATIVE) that the loader writes once, so the linker puts the
/// array in .data.rel.ro, inside PT_GNU_RELRO, rather than in .rodata.
constexpr char const *RELRO_WORDS[] = {"written", "once", "by", "the", "loader"};

/// A store into |RELRO_WORDS|, through a pointer that casts its const away:
/// its first entry, written back with what it holds, so that a store that
/// goes through changes nothing. The page is wholly RELRO -- LLD pads RELRO
/// to a page -- and so mapped read-only. #PF, present, user write. While the
/// kernel mapped a program as one range, read, write and execute, it went
/// through.
auto pf_write_relro(ours::VirtAddr) -> void {
    auto *const entry = const_cast<char const **>(&RELRO_WORDS[0]);
    __asm__ volatile("movq (%0), %%rax\n\t"
                     "movq %%rax, (%0)"
                     :
                     : "r"(entry)
                     : "rax", "memory");
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

constexpr Probe PROBES[] = {
    {"pf_unmapped", pf_unmapped},
    {"pf_kernel", pf_kernel},
    {"pf_write_vdso", pf_write_vdso},
    {"pf_exec_vdso_data", pf_exec_vdso_data},
    {"pf_write_text", pf_write_text},
    {"pf_exec_data", pf_exec_data},
    {"pf_write_relro", pf_write_relro},
    {"ud2", ud2},
    {"gp_hlt", gp_hlt},
    {"gp_int80", gp_int80},
    {"gp_int3", gp_int3},
    {"de", de},
    {"db_tf", db_tf},
    {"gp_io", gp_io},
    {"trap_outside_vdso", trap_outside_vdso},
};

/// What every entry of |relocated| points at.
constexpr char TAG[] = "relocated";

/// More than two pages of pointers in writable data, each one an
/// R_X86_64_RELATIVE relocation, with .bss after them. That is the one shape
/// the K23 launcher maps partly from a snapshot of the program's file: the
/// whole pages before the one its data ends in, and it writes these
/// relocations into that snapshot. No other program in the tree has a page
/// of writable file bytes, so without this table the boot never takes that
/// path. A wrong page, or a relocation missed or written elsewhere, leaves an
/// entry that does not point at TAG.
///
/// constinit, because nothing runs a dynamic initialiser in this program: the
/// runtime has no .init_array, and the table must be built at link time, as
/// relocations, or not at all.
constexpr usize RELOCATED_COUNT = 1100;

struct Relocated {
    char const *entries[RELOCATED_COUNT];

    constexpr Relocated() : entries{} {
        for (auto &entry: entries) {
            entry = TAG;
        }
    }
};

constinit Relocated relocated;

/// Whether every entry of |relocated| points at TAG. Read through volatile:
/// the compiler can see every entry was set to TAG, and would otherwise
/// answer for the memory without reading it.
auto relocations_hold() -> bool {
    for (auto const &entry: relocated.entries) {
        char const *const volatile *const at = &entry;
        if (*at != TAG) {
            return false;
        }
    }
    return true;
}

auto same(std::string_view const a, std::string_view const b) -> bool {
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

/// What main returns, which the runtime passes to process_exit and a debug
/// kernel prints as the process's retcode. Distinct, so that the log alone
/// says how a probe that was not killed ended.
enum ExitCode : int {
    /// The probe's instruction did not fault.
    SURVIVED = 1,
    /// No probe has that name -- the driver's control run.
    UNKNOWN = 2,
    /// Started without a probe name.
    NO_NAME = 3,
    /// A pointer in RELOCATED does not point at TAG: the loader got this
    /// program's writable data wrong.
    BAD_RELOCATION = 4,
};

/// Says |answer| to the driver on |channel|, the User0 startup handle.
/// Without one there is no one to tell, and the exit code has to do.
auto reply(ours::Uandle const channel, std::string_view const answer) -> void {
    if (channel == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("fault_probe: no answer channel (User0)");
        return;
    }
    auto _ = ours::channel_write(channel, ours::ChannelWriteOptions::None, answer.data(),
                                 static_cast<u32>(answer.size()), nullptr, 0);
}
} // namespace

auto main(int const argc, char **const argv) -> int {
    auto const answer = ort::take_startup_handle(ours::HandleType::User0, 0);
    if (!relocations_hold()) {
        auto _ = ort::debug_line("fault_probe: a pointer in its writable data was not "
                                 "relocated where it lies");
        reply(answer, "relocation");
        return BAD_RELOCATION;
    }
    if (argc < 2) {
        auto _ = ort::debug_line("fault_probe: no probe name");
        reply(answer, "no probe name");
        return NO_NAME;
    }
    // Up to its NUL or 32 bytes, whichever comes first: no probe name is
    // longer, and the argument came from a message another program wrote.
    auto const *const arg = argv[1];
    auto const name =
        std::string_view{arg, static_cast<usize>(std::find(arg, arg + 32, '\0') - arg)};
    auto _ = ort::debug_line({"fault_probe: ", name});

    for (auto const &probe: PROBES) {
        if (same(name, probe.name)) {
            probe.action(ort::vdso_base());
            reply(answer, "survived");
            return SURVIVED;
        }
    }
    auto _ = ort::debug_line({"fault_probe: unknown probe ", name});
    reply(answer, "unknown");
    return UNKNOWN;
}
