/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_USERBOOT_HPP
#define USERABI_USERBOOT_HPP 1

namespace userabi {

/// Where a user process's handle indices live once the handoff protocol
/// settles.
enum class HandleIndex {};

/// Brings up the first user process: decompresses the kernel package,
/// builds the user BootFS from it, then launches the userboot
/// program (`kernel.select.userboot` picks which build, "userboot" by
/// default) with a bootstrap channel whose processargs message hands it the
/// root job, the BootFS and the image's user-space options (`userboot.*`,
/// `ours.autorun.*`). What runs after
/// userboot is userboot's decision (`userboot.next`), not this function's.
auto userboot_init() -> void;

} // namespace userabi

#endif // #ifndef USERABI_USERBOOT_HPP
