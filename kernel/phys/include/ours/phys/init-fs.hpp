/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_INIT_FS_HPP
#define OURS_PHYS_INIT_FS_HPP 1

#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace ours::phys {
    // FIXME(SmallHuaZi) Temporary type alias for maintaining the consistency of interface.
    // It should be removed after providing the implementation of `InitFs`
    using InitFs = ustl::views::Span<u8>;
} // namespace ours::phys

#endif // #ifndef OURS_PHYS_INIT_FS_HPP 