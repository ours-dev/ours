/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <gktl/command.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
namespace {
    /// The registry's bounds, as the linker computes them.
    ///
    /// kernel/kernel.ld puts every GKTL_COMMAND record between these two, so
    /// walking them is the whole of "what commands exist" -- the same source
    /// gktl::run_command() builds its radix tree from
    /// (kernel/lib/gktl/src/command.cpp), rather than a second list that could
    /// disagree with it.
    extern "C" gktl::Command const COMMANDS_START[] LINK_NAME("__commands_start");
    extern "C" gktl::Command const COMMANDS_END[] LINK_NAME("__commands_end");

    /// Appends |text| at |at|, never past |limit|, and reports where it got to.
    ///
    /// Returns the *would-be* end even when it ran out of room, so the caller
    /// can tell "filled exactly" from "truncated" without a second length pass.
    auto append(char *at, char const *const limit, char const *text) -> char * {
        if (text == nullptr) {
            return at;
        }
        while (*text != '\0') {
            if (at < limit) {
                *at = *text;
            }
            ++at;
            ++text;
        }
        return at;
    }
} // namespace

auto sys_command_list(char *const buffer, usize const length) -> Status {
    if (buffer == nullptr || length == 0) {
        return Status::InvArgs;
    }

    // The same range check sys_debug_write() makes, and for the same reason:
    // a buffer that starts in user space and runs past the split would let a
    // user program have the kernel write over its own memory. The end is
    // computed as a length rather than as `start + length`, because that sum
    // can wrap and a wrapped pointer passes a naive check by landing back
    // below the split.
    //
    // This one writes rather than reads, so getting it wrong is worse. Same
    // caveat as sys_debug_write: without a copy_to_user that can survive the
    // page being unmapped underneath it, this is a check plus a direct store,
    // and the fault handler that would make it honest does not exist yet.
    auto const start = reinterpret_cast<VirtAddr>(buffer);
    if (mm::is_kernel_address(start) || length > VirtAddr(-1) - start ||
        mm::is_kernel_address(start + length - 1)) {
        return Status::InvArgs;
    }

    // One byte is reserved up front so the result is always terminated, even
    // when it is cut short. A caller that gets an un-terminated buffer back has
    // no way to tell where the text ends.
    auto *const limit = buffer + length - 1;
    auto *at = buffer;

    for (auto const *command = COMMANDS_START; command != COMMANDS_END; ++command) {
        if (command->cmd == nullptr) {
            continue;
        }
        at = append(at, limit, command->cmd);
        if (command->help != nullptr && command->help[0] != '\0') {
            at = append(at, limit, "  ");
            at = append(at, limit, command->help);
        }
        at = append(at, limit, "\n");
    }

    if (at > limit) {
        *limit = '\0';
        return Status::NoSpace;
    }
    *at = '\0';
    return Status::Ok;
}
} // namespace ours
