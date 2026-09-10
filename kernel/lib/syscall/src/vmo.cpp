/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/object/handle.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
// Declarations only -- the VMO object type does not exist yet. Pointers for
// the same reason as task.cpp: object::Handle has no public constructor and
// no copy, so it cannot cross a by-value signature.
auto vmo_create(object::Handle *handle) -> object::Handle *;

auto vmo_create_child(object::Handle *handle) -> object::Handle *;
} // namespace ours::syscall
