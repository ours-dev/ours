/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_OPTIONS_HPP
#define ARCH_PAGING_OPTIONS_HPP 1

#include <arch/types.hpp>

#include <ustl/util/pack_options.hpp>

namespace arch::paging {
USTL_TYPE_OPTION(PageAllocator, PageAllocator);

USTL_CONSTANT_OPTION(PagingLevel, usize, PAGING_LEVEL);

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_OPTIONS_HPP