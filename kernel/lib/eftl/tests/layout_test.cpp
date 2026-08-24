/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/details/dyn.hpp>
#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/details/structures.hpp>
#include <eftl/elf/details/types.hpp>

#include <cstddef>

static_assert(sizeof(eftl::elf::Elf64Half) == sizeof(eftl::elf::u16));
static_assert(sizeof(eftl::elf::Elf64Word) == sizeof(eftl::elf::u32));
static_assert(sizeof(eftl::elf::Elf64Ehdr) == 64);
static_assert(sizeof(eftl::elf::Elf64Phdr) == 56);
static_assert(offsetof(eftl::elf::Elf64Ehdr, e_entry) == 24);
static_assert(offsetof(eftl::elf::Elf64Phdr, p_offset) == 8);
static_assert(sizeof(eftl::elf::Elf64Dyn) == 16);

auto main() -> int {
    return 0;
}
