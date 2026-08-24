/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_DETAILS_TYPES_HPP
#define EFTL_ELF_DETAILS_TYPES_HPP 1

#include <eftl/elf/config.hpp>

#include <ours/types.hpp>

namespace eftl::elf {
using Elf32Addr = u32;
using Elf32Off = u32;
using Elf32Half = u16;
using Elf32Word = u32;
using Elf32Sword = i32;

using Elf64Addr = u64;
using Elf64Off = u64;
using Elf64Half = u16;
using Elf64Word = u32;
using Elf64Sword = i32;

using Elf64Xword = u64;
using Elf64Sxword = i64;

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_DETAILS_TYPES_HPP
