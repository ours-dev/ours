/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTMEM_TYPES_HPP
#define BOOTMEM_TYPES_HPP 1

#include <limits>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace bootmem {
using ours::NnId;
using ours::PhysAddr;
using ours::Status;

inline constexpr auto ANY_NODE = std::numeric_limits<NnId>::max();

struct Range final {
    PhysAddr base{};
    usize size{};

    [[nodiscard]]
    static constexpr auto from(PhysAddr const base, usize const size)
        -> ustl::Result<Range, Status> {
        if (size == 0) {
            return ustl::err(Status::InvArgs);
        }
        if (base > std::numeric_limits<PhysAddr>::max() - size) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(Range{.base = base, .size = size});
    }

    [[nodiscard]]
    constexpr auto end() const -> PhysAddr {
        return base + size;
    }
};

struct MemoryRegion final {
    Range range{};
    NnId node{};
};

/// What a range of physical memory is, and -- when it is reserved -- what it is
/// reserved *for*.
///
/// A reservation used to be a bare Range, so every reserved range looked alike:
/// the phys image, a page table the kernel is about to walk, a hole the firmware
/// declared and a scratch buffer nobody needs any more were one and the same
/// fact. That is the reason the kernel could not tell a live page table from a
/// gap in RAM, and the reason its frame descriptors could only ever be stamped
/// "reserved" (mm::init_memmap_reserved()) instead of saying what they hold.
///
/// One enum rather than a bool plus a purpose, because two fields can disagree:
/// `Free` is a value here, so a range cannot be both free and a page table. The
/// zero value is deliberately `Unknown` and not `Free` -- a default-constructed
/// or memset range reads as "reserved, purpose unrecorded", which is the safe
/// direction to be wrong in.
///
/// Zircon's counterpart is memalloc::Type (kernel/phys/lib/memalloc), which
/// carries the same information across the same boundary; its kFreeRam,
/// kKernelPageTables, kDataZbi and kPhysScratch are Free, PageTable,
/// BootPackage and Transient here.
enum class MemoryType : u8 {
    /// Reserved, purpose not recorded. Nothing should produce this once every
    /// reservation site names its type; it exists so that a range whose bytes
    /// were never set does not read as free.
    Unknown,

    /// Usable RAM that nobody has claimed.
    Free,

    /// The firmware said this is not RAM.
    FirmwareReserved,

    /// Memory-mapped device registers.
    Peripheral,

    /// The phys program's own image: physload and the modules it loaded.
    PhysImage,

    /// The OBI boot package, where the bootloader placed it.
    BootPackage,

    /// The kernel image, as physboot loaded it.
    KernelImage,

    /// Page tables describing mappings that outlive the phys program -- the
    /// tables the kernel goes on walking.
    PageTable,

    /// The handoff structures themselves, and anything the handoff has to keep
    /// alive across the jump into the kernel.
    Handoff,

    /// The kernel's own early bookkeeping, kept for the life of the kernel:
    /// node and zone descriptors, the frame memmap.
    KernelData,

    /// Reserved early and handed back to the page allocator once its one user is
    /// done with it -- the boot CPU's provisional per-CPU unit, the AP
    /// trampoline buffer. Distinct from KernelData because these frames *do*
    /// become the buddy's, and from Free because until then they must not.
    Transient,

    /// Given to the page allocator at EarlyMem::do_handoff(): the buddy owns it
    /// now, and bootmem must never hand it out again.
    ///
    /// This is what the handover does to a Free range, and the reason the
    /// handover is expressible at all. It used to be that bootmem released its
    /// free ranges to the buddy and then went on calling them free, so the two
    /// allocators both believed they owned the same frames and the second taker
    /// wrote over the first. Naming the state ends that: after the handover
    /// nothing in bootmem is Free, so find_free() finds nothing.
    PageAllocator,
};

[[nodiscard]]
constexpr auto to_string(MemoryType const type) -> char const * {
    switch (type) {
        case MemoryType::Unknown:
            return "unknown";
        case MemoryType::Free:
            return "free";
        case MemoryType::FirmwareReserved:
            return "firmware-reserved";
        case MemoryType::Peripheral:
            return "peripheral";
        case MemoryType::PhysImage:
            return "phys image";
        case MemoryType::BootPackage:
            return "boot package";
        case MemoryType::KernelImage:
            return "kernel image";
        case MemoryType::PageTable:
            return "page table";
        case MemoryType::Handoff:
            return "handoff";
        case MemoryType::KernelData:
            return "kernel data";
        case MemoryType::Transient:
            return "transient";
        case MemoryType::PageAllocator:
            return "page allocator";
    }
    return "invalid";
}

/// One reserved range and what it is for.
struct Reservation final {
    Range range{};
    MemoryType type{MemoryType::Unknown};
};

/// One range plus what it is, for handing the whole memory map to a consumer
/// that should read it rather than share this allocator's live state -- see
/// PhysicalMemory::snapshot() and ours::phys::MemoryHandoff.
struct MemRange final {
    Range range{};
    MemoryType type{MemoryType::Unknown};

    /// Whether the range is claimed. `type` says by whom; this is for the
    /// callers that only need to know whether the page allocator may have it.
    [[nodiscard]]
    constexpr auto used() const -> bool {
        return type != MemoryType::Free;
    }
};

enum class AllocationDirection : u8 {
    BottomUp,
    TopDown,
};

struct AllocationRequest final {
    usize size{};
    usize alignment{};
    PhysAddr lower{};
    PhysAddr upper{};
    NnId node{ANY_NODE};
    AllocationDirection direction{AllocationDirection::BottomUp};
    /// What the allocation is for; recorded on the reservation it creates.
    MemoryType type{MemoryType::Unknown};
};

enum class MemoryView : u8 {
    Usable,
    Free,
};

struct MemoryQuery final {
    PhysAddr lower{};
    PhysAddr upper{};
    NnId node{ANY_NODE};
    MemoryView view{MemoryView::Usable};
};

struct PhysicalMemoryStorage final {
    ustl::views::Span<MemoryRegion> usable{};
    ustl::views::Span<Reservation> reservations{};
};

using MemoryVisitor = auto (*)(void *, MemoryRegion) -> bool;

} // namespace bootmem

namespace bootmm = bootmem;

#endif // #ifndef BOOTMEM_TYPES_HPP
