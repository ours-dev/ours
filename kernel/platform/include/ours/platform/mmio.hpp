/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_MMIO_HPP
#define OURS_PLATFORM_MMIO_HPP 1

#include <ours/types.hpp>

#include <ustl/views/span.hpp>

namespace ours::platform {
/// A run of physical addresses, [base, base + size).
struct MmioRange {
    u64 base;
    u64 size;
};

/// Records that the kernel drives the device registers at [base, base + size)
/// itself, so that touches_kernel_physical() answers yes for them from now on.
///
/// A platform's init code calls it once for each device it maps, before the
/// first user program runs: it is not safe against a concurrent call, nor
/// against a reader. The range grows to whole pages, as a mapping of it
/// does, because a program that mapped the page beside a register would reach
/// the register all the same.
///
/// There is room for a few dozen devices. A platform that names more does not
/// stop the boot: the ones past the end cannot be recorded, so
/// touches_kernel_physical() answers yes for every address from then on, and
/// no MmIo resource grants anything.
auto reserve_kernel_mmio(u64 base, u64 size) -> void;

/// The device registers the platform has reserved so far, in the order it
/// reserved them and in whole pages. A platform that maps no device of its own
/// answers an empty list.
[[nodiscard]]
auto kernel_mmio() -> ustl::views::Span<MmioRange const>;

/// The RAM the memory manager knew at boot, claimed or free, in whole pages
/// and in address order. Empty until it has been copied, which happens while
/// the kernel initialises, and if it could not be copied.
///
/// It is a copy kept in ordinary static storage. The map it comes from lives
/// in the init sections, which the page allocator owns once boot is over.
[[nodiscard]]
auto kernel_ram() -> ustl::views::Span<MmioRange const>;

/// Whether [base, base + size) shares an address with something the kernel
/// owns, which no MmIo resource grants, not even the root one:
///
///   - RAM, claimed or free: every run kernel_ram() lists. A program that
///     mapped it could read or rewrite the kernel's own data and every other
///     process's. Until the RAM has been copied, and if that failed, every
///     address counts as RAM;
///   - the device registers kernel_mmio() lists: a program writing to one
///     would change a device's state behind the back of the driver that keeps
///     it, as with kernel_ioports().
///
/// An empty request shares no address, and so touches nothing. The caller has
/// checked that the request lies inside the physical address space, so
/// base + size cannot overflow.
[[nodiscard]]
auto touches_kernel_physical(u64 base, u64 size) -> bool;
} // namespace ours::platform

#endif // #ifndef OURS_PLATFORM_MMIO_HPP
