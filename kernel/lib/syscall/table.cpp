/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/compiler.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>

namespace ours {
namespace {
    /// x86_syscall (arch/x86/kernel/syscall.S) pushes the user registers and
    /// then does `call *syscall_table(, %rax, 8)` with %rdi holding a pointer
    /// to that frame. It restores %rax from the frame afterwards, so a handler
    /// answers by writing into the frame rather than by returning a value.
    ///
    /// The frame is deliberately opaque here: no type describes that push
    /// order anywhere in the tree yet, and guessing at one would be worse than
    /// admitting it.
    using SyscallHandler = auto (*)(void *frame) -> void;

    auto sys_not_implemented(void *) -> void {}

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

NO_MANGLE constinit auto syscall_table = make_syscall_table();

} // namespace ours
