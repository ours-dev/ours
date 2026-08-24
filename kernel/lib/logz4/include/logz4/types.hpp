/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_TYPES_HPP
#define LOGZ4_TYPES_HPP 1

namespace logz4 {
using i8 = ::std::int8_t;
using i16 = ::std::int16_t;
using i32 = ::std::int32_t;
using i64 = ::std::int64_t;
using isize = ::std::ptrdiff_t;

using u8 = ::std::uint8_t;
using u16 = ::std::uint16_t;
using u32 = ::std::uint32_t;
using u64 = ::std::uint64_t;
using usize = ::std::size_t;

using VirtAddr = usize;
using PhysAddr = usize;
using PgOff = isize;

using CpuNum = u32;
using HIrqNum = u32;
using VIrqNum = u32;
using NnId = u32;   // Numa Node Id
using KoId = u32;   // Kernel Object Id
using Handle = u32; // Kernel Object Handle
} // namespace logz4

#endif // #ifndef LOGZ4_TYPES_HPP