/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <arch/syscall.hpp>
#include <ours/compiler.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

#include <array>

namespace ours {
namespace {
    /// x86_syscall (arch/x86/kernel/syscall.S) saves the user registers into
    /// an arch::SyscallFrame on the kernel stack and then does
    /// `call *SYSCALL_TABLE(, %rax, 8)` with %rdi holding a pointer to it. It
    /// pops the whole frame back into the registers afterwards, so a handler
    /// answers by writing `frame->ax` rather than by returning a value -- a
    /// C++ return value would be overwritten on the way out.
    ///
    /// The frame used to be spelled `void *` here, because nothing in the tree
    /// described the stub's push order. arch::SyscallFrame does now
    /// (arch/x86/syscall.hpp), and it is static_asserted against the same
    /// offset macros the stub is written from.
    using SyscallHandler = auto (*)(arch::SyscallFrame *frame) -> void;

    auto sys_not_implemented(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::Unimplemented);
    }

    /// The answer to a system-call number that does not exist at all, as
    /// against one that exists and has no handler yet.
    ///
    /// It is an ordinary table entry rather than a branch target in the stub,
    /// which is what lets the range check be a `cmov` instead of a
    /// conditional jump: the stub clamps an out-of-range number onto this
    /// slot's index and calls through the table either way.
    auto sys_bad_syscall_number(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::OutOfRange);
    }

    /// The table x86_syscall indexes. %rax is user-controlled, so every slot
    /// has to be a valid target -- a null entry would be an indirect call
    /// through zero from user space.
    ///
    /// One entry longer than SYSCALL_COUNT. The extra slot at the end is the
    /// out-of-range answer, and it exists so the stub's range check can clamp
    /// rather than branch: a number at or above SYSCALL_COUNT is rewritten to
    /// this index and dispatched like any other. Without it the stub would
    /// have to jump around the call and set the return value in assembly,
    /// which means spelling a Status constant there.
    ///
    /// TODO(SmallHuaZi): every in-range slot still answers Unimplemented,
    /// because there is no syscall numbering yet to give the slots meaning and
    /// nothing unpacks the frame into lib/syscall's typed sys_* entry points.
    constexpr auto TABLE_SIZE = SYSCALL_COUNT + 1;

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, TABLE_SIZE> table{};
        for (auto &entry: table) {
            entry = &sys_not_implemented;
        }
        table[SYSCALL_COUNT] = &sys_bad_syscall_number;
        return table;
    }
} // namespace

extern "C" constinit auto SYSCALL_TABLE = make_syscall_table();

} // namespace ours
