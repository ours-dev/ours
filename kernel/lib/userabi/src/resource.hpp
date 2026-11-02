/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_SRC_RESOURCE_HPP
#define USERABI_SRC_RESOURCE_HPP 1

#include <ours/object/handle.hpp>
#include <ours/resource.hpp>
#include <ours/types.hpp>
#include <ustl/box.hpp>

namespace userabi {
/// A handle to the System resource slot at |base| (one of the
/// ours::RSRC_SYSTEM_*_BASE values), with the default resource rights; null,
/// after saying why, when it cannot be made.
///
/// The kernel mints these at boot for the first process and nowhere else: the
/// slot a resource names is the capability, and only what the kernel hands out
/// here exists.
auto make_system_resource_handle(u64 base, char const *name) -> ustl::Box<ours::object::Handle>;

/// A handle to the root resource of the ranged |kind| (Irq or IoPort): base 0
/// and size 0, covering every interrupt line or every I/O port. It grants
/// none of the ports the kernel drives itself (validate_ranged_resource()),
/// and a line the kernel is on refuses any other attach. Default resource
/// rights; null, after saying why, when it cannot be made.
///
/// Minted at boot for the first process only, as the System slots are, so
/// that whoever holds it decides which driver gets which lines and ports.
auto make_root_resource_handle(ours::RsrcKind kind, char const *name)
    -> ustl::Box<ours::object::Handle>;
} // namespace userabi

#endif // USERABI_SRC_RESOURCE_HPP
