/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_PROCESS_HPP
#define OU_PROCESS_HPP 1

#include <ou/str.hpp>

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ou {
/// Ends this process with |retcode|. Every thread in it is killed; this one
/// exits here and never comes back.
[[noreturn]]
auto process_exit(i64 retcode) -> void;

/// TEMPORARY -- goes when a user-space loader exists.
///
/// Starts |name| from the kernel package as a new process under |job| (which
/// must grant ManageProcess), with |bootstrap| as its one handle. |bootstrap|
/// is gone from this process when this returns, whether or not it succeeded.
/// NotFound when the package has no such program.
auto process_spawn_boot(ours::HandleValue job, Str name, ours::HandleValue bootstrap)
    -> ours::Status;
} // namespace ou

#endif // #ifndef OU_PROCESS_HPP
