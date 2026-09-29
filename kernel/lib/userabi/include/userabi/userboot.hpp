/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_USERBOOT_HPP
#define USERABI_USERBOOT_HPP 1

#include <string_view>

#include <ours/object/handle.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/status.hpp>
#include <ustl/box.hpp>
#include <ustl/rc.hpp>

namespace userabi {

/// Where a user process's handle indices live once the handoff protocol
/// settles.
enum class HandleIndex {};

/// Brings up the first user process: decompresses the kernel package and
/// keeps it, builds the user BootFS from it, then launches the userboot
/// program (`kernel.select.userboot` picks which build, "userboot" by
/// default) with a bootstrap channel whose processargs message hands it the
/// root job, the BootFS and the image's user-space options (`userboot.*`,
/// `ours.autorun.*`). What runs after
/// userboot is userboot's decision (`userboot.next`), not this function's.
auto userboot_init() -> void;

/// Starts the program |name| from the kernel package as a new process under
/// |job|, with |bootstrap| (which may be empty) as the one handle it holds;
/// the program receives the handle's value in %rdi and its vDSO's base in
/// %rsi.
///
/// Maps the image and the vDSO, binds the image's imports to the vDSO,
/// records the vDSO range, gives it a 64 KiB stack, and starts its thread --
/// the same sequence for the first program and for any started later.
///
/// A handle to the new process's root VMA goes into |root_vma|, when it is
/// not null, for the caller to put in the processargs it writes on the other
/// end of |bootstrap| -- after this returns, since the region only exists
/// from here on. TEMPORARY in this form, until K23 lets a user-space launcher
/// create the process itself (process_create), as Zircon's does.
///
/// Only files in the kernel package, which the kernel itself loaded: nothing
/// here parses ELF a user program supplied.
auto launch(ustl::Arc<ours::object::JobDispatcher> job, std::string_view name,
            ustl::Box<ours::object::Handle> bootstrap, ustl::Box<ours::object::Handle> *root_vma)
    -> ours::Status;

} // namespace userabi

#endif // #ifndef USERABI_USERBOOT_HPP
