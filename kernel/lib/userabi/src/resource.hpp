/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_SRC_RESOURCE_HPP
#define USERABI_SRC_RESOURCE_HPP 1

#include <ours/object/handle.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ustl/box.hpp>

namespace userabi {
/// A handle to the System resource slot at |base| (one of the
/// ours::RSRC_SYSTEM_*_BASE values), with the default resource rights; null,
/// after saying why, when it cannot be made.
///
/// The kernel mints these at boot for the first process and nowhere else: the
/// slot a resource names is the capability, and only what the kernel hands out
/// here exists. Zircon's userboot does the same with get_resource_handle()
/// and hands the result on in its bootstrap message.
auto make_system_resource_handle(u64 base, char const *name) -> ustl::Box<ours::object::Handle>;
} // namespace userabi

#endif // USERABI_SRC_RESOURCE_HPP
