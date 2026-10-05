/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_USERBOOT_HPP
#define USERABI_USERBOOT_HPP 1

#include <string_view>

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

/// Keeps `text`, the crash record the previous boot left as the kernel
/// recovered it, for the first user process: userboot_init() hands it over as
/// a read-only VMO (HandleType::VmoCrashlog). The text is copied into a fresh
/// VMO at once, so the caller may clear the region it came from. Called
/// before userboot_init(), on the same thread.
auto stash_crashlog(std::string_view text) -> void;

} // namespace userabi

#endif // #ifndef USERABI_USERBOOT_HPP
