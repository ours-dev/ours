/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_USERBOOT_HPP
#define USERABI_USERBOOT_HPP 1

namespace userabi {

/// Where a user process's handle indices live once the handoff protocol
/// settles.
enum class HandleIndex {};

/// Brings up userboot, the first user process. Forward-development stub:
/// creates the process object; the handle-table registration and the ELF
/// loading that follow have no implementation yet.
auto userboot_init() -> void;

} // namespace userabi

#endif // #ifndef USERABI_USERBOOT_HPP
