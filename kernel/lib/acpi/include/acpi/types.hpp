/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_TYPES_HPP
#define ACPI_TYPES_HPP 1

#include <cstdint>
#include <cstddef>

#include <ours/compiler.hpp>
#include <ours/status.hpp>

namespace acpi {
    using i8    = ::int8_t;
    using i16   = ::int16_t;
    using i32   = ::int32_t;
    using i64   = ::int64_t;
    using isize = ::ptrdiff_t;

    using u8    = ::uint8_t;
    using u16   = ::uint16_t;
    using u32   = ::uint32_t;
    using u64   = ::uint64_t;
    using usize = ::size_t;

    using VirtAddr  = usize;
    using PhysAddr  = usize;

    using ours::Status;
} // namespace acpi

#endif // #ifndef ACPI_TYPES_HPP