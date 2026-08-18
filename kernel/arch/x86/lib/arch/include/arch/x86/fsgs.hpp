/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_FSGS_HPP
#define ARCH_X86_FSGS_HPP 1

#include <arch/types.hpp>

namespace arch {

template <typename Integer>
inline auto read_gs_offset(usize offset) -> Integer {
    Integer value;
    auto const *address = reinterpret_cast<Integer const *>(offset);
    asm volatile("mov %%gs:%1, %0" : "=r"(value) : "m"(*address) : "memory");
    return value;
}

template <typename Integer>
inline auto write_gs_offset(usize offset, Integer value) -> void {
    auto *address = reinterpret_cast<Integer *>(offset);
    asm volatile("mov %0, %%gs:%1" : : "ir"(value), "m"(*address) : "memory");
}

template <typename Integer>
inline auto read_fsbase() -> Integer {
    Integer value;
    asm volatile("rdfsbase %0" : "=r"(value) : : "memory");
    return value;
}

template <typename Integer>
inline auto write_fsbase(Integer value) -> void {
    asm volatile("wrfsbase %0" : : "r"(value) : "memory");
}

template <typename Integer>
inline auto read_gsbase() -> Integer {
    Integer value;
    asm volatile("rdgsbase %0" : "=r"(value) : : "memory");
    return value;
}

template <typename Integer>
inline auto write_gsbase(Integer value) -> void {
    asm volatile("wrgsbase %0" : : "r"(value) : "memory");
}

inline auto swap_gs() -> void {
    asm volatile("swapgs" : : : "memory");
}

template <typename Integer>
inline auto exchange_gsbase_after_swap(Integer const value) -> Integer {
    swap_gs();
    auto const previous = read_gsbase<Integer>();
    write_gsbase(value);
    swap_gs();
    return previous;
}

} // namespace arch

#endif // ARCH_X86_FSGS_HPP
