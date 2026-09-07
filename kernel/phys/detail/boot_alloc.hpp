/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_DETAIL_BOOT_ALLOC_HPP
#define OURS_PHYS_DETAIL_BOOT_ALLOC_HPP 1

#include <ours/panic.hpp>
#include <ours/phys/arch_bootmem.hpp>
#include <ours/phys/init.hpp>
#include <ours/types.hpp>

#include <string_view>

namespace ours::phys::detail {

/// Top-down allocate-or-panic through the live bootmem, spanning its full
/// range; `what` names the allocation in the panic message.  The two
/// spellings this replaces (module_loader.cpp's bootmem_allocate and
/// physboot.cpp's alloc_top_down) lived in different ELF images: physboot
/// is a physload_module that links none of physload.core's objects and
/// binds against its export table instead, so this must stay header-only
/// inline -- an out-of-line body would hand the module a hidden undefined
/// symbol and fail its link.
[[nodiscard]]
inline auto allocate_top_down(usize const size, usize const alignment, std::string_view const what)
    -> bootmem::Range {
    auto const mem = global_bootmem();
    auto const allocation = mem->allocate({
        .size = size,
        .alignment = alignment,
        .lower = mem->min_address(),
        .upper = mem->max_address(),
        .node = bootmem::ANY_NODE,
        .direction = bootmem::AllocationDirection::TopDown,
    });
    if (allocation.is_err()) {
        panic("No memory to place {}", what);
    }
    return allocation.unwrap();
}

} // namespace ours::phys::detail

#endif // #ifndef OURS_PHYS_DETAIL_BOOT_ALLOC_HPP
