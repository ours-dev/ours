/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <arch/syscall.hpp>
#include <ours/compiler.hpp>
#include <ours/status.hpp>
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

    /// The table x86_syscall indexes. %rax is user-controlled, so every slot
    /// has to be a valid target -- a null entry would be an indirect call
    /// through zero from user space.
    ///
    /// TODO(SmallHuaZi): every slot answers the same way, because the dispatch
    /// layer does not exist. lib/syscall's sys_* entry points take typed
    /// arguments (Handle, VirtAddr, ...) and nothing unpacks the register
    /// frame into them; a real table also needs the syscall numbering that
    /// would give these slots their meaning. Until then this exists so the
    /// kernel links and so a stray syscall returns instead of jumping to zero.
    constexpr auto NUM_SYSCALLS = usize{256};

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, NUM_SYSCALLS> table{};
        for (auto &entry: table) {
            entry = &sys_not_implemented;
        }
        return table;
    }
} // namespace

extern "C" constinit auto SYSCALL_TABLE = make_syscall_table();

} // namespace ours
