/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_VISITORS_HPP
#define ARCH_PAGING_VISITORS_HPP 1

#include <ours/compiler.hpp>

#include <arch/paging/mmu_flags.hpp>
#include <arch/types.hpp>

namespace arch::paging {
struct QueryResult {
    PhysAddr phys;
    usize size;
    MmuFlags flags;
};

enum class MapError {
    InvArgs,
    Existent,
    NoMem,
};

FORCE_INLINE
constexpr auto to_string(MapError error) -> char const * {
    switch (error) {
        case MapError::InvArgs:
            return "Invalid argument";
        case MapError::Existent:
            return "Existent";
        case MapError::NoMem:
            return "No memory";
    }

    return "Unknown map error";
}

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_VISITORS_HPP
