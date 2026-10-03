/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_PROCESS_HPP
#define OU_PROCESS_HPP 1

#include <ou/str.hpp>

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
/// Ends this process with |retcode|. Every thread in it is killed; this one
/// exits here and never comes back.
[[noreturn]]
auto process_exit(i64 retcode) -> void;

/// Makes a process under |job| (which must grant ManageProcess) that is not
/// running yet: the handle to it goes in |process|, the handle to its root VMA
/// in |root_vma|. Closing the last handle to it before it is started destroys
/// it. |options| must be None (InvArgs otherwise), and is passed through as
/// given. Zircon zx_process_create().
auto process_create(ours::HandleValue job, Str name, ours::HandleValue *process,
                    ours::HandleValue *root_vma,
                    ours::ProcessCreateOptions options = ours::ProcessCreateOptions::None)
    -> ours::Status;

/// Starts |process| with |thread| as its first thread, at |entry| with |stack|.
/// |arg1|, the one handle the process starts with (HANDLE_INVALID for none),
/// moves into the new process and its value there is passed in %rdi; |arg2| in
/// %rsi. |arg1| is gone from this process whether or not the start worked.
/// Zircon zx_process_start().
auto process_start(ours::HandleValue process, ours::HandleValue thread, ours::VirtAddr entry,
                   ours::VirtAddr stack, ours::HandleValue arg1, usize arg2) -> ours::Status;

/// TEMPORARY -- goes once its callers start programs with the user-space
/// launcher (system/lib/launch) instead.
///
/// Starts |name| from the kernel package as a new process under |job| (which
/// must grant ManageProcess), with |bootstrap| (which must grant Transfer) as
/// its one handle. |bootstrap| is gone from this process when this returns,
/// whether or not it succeeded. NotFound when the package has no such program.
///
/// The child's root VMA comes back in |root_vma|. The caller writes the
/// child's processargs on its end of the bootstrap channel afterwards, with
/// |root_vma| in it as VmaRoot: the child waits for that message.
auto process_spawn_boot(ours::HandleValue job, Str name, ours::HandleValue bootstrap,
                        ours::HandleValue *root_vma) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_PROCESS_HPP
