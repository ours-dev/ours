/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTSHIM_DEVICETREE_HPP
#define BOOTSHIM_DEVICETREE_HPP 1

#include <bootshim/conversion.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string_view>

#include <fdt/tree.hpp>
#include <obi/package_format.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

/// What a boot-shim takes from a flattened device tree: the memory map, in the
/// obi::MemoryRange vocabulary physload already reads (through
/// LegacyPlatformInfo::memory_ranges and arch_detect_memory()), and the two
/// facts /chosen carries for the next stage, the initrd and the command line.
///
/// The device-tree counterpart of legacy_memory.hpp, kept out of the riscv
/// shim for the same reason: none of it needs a machine, so a host test can
/// feed it trees no firmware would hand over. Like that header it neither
/// prints nor panics; it reports, and the shim decides how loud to be.
namespace bootshim {
using ours::Status;

/// The most ranges one conversion writes. A device tree describes a few: one
/// RAM range per memory bank, one per firmware region, and the few the shim
/// adds for itself. The array lives in the shim's image for as long as
/// physload reads it, because nothing can allocate before physload exists.
inline constexpr auto MAX_DEVICETREE_MEMORY_RANGES = usize{64};

using DevicetreeMemoryRanges = std::array<obi::MemoryRange, MAX_DEVICETREE_MEMORY_RANGES>;

/// The physical range [base, base + size).
struct PhysRange final {
    u64 base;
    u64 size;
};

/// Turns one device tree into the memory map physload receives, and reads the
/// initrd and the command line out of /chosen.
///
/// The map, in this order:
/// - Ram: every `reg` entry of every child of the root whose `device_type` is
///   "memory", decoded with the root's #address-cells and #size-cells. Only
///   the root's children: the binding puts memory nodes there, and a node
///   further down would need its bus's `ranges` applied to reach a CPU
///   address, which this does not do.
/// - Reserved: every entry of the memory reservation block, less the initrd
///   (below).
/// - Reserved: every `reg` entry of every child of /reserved-memory, decoded
///   with /reserved-memory's own cell widths. A child with no `reg` asks for
///   memory to be allocated for it rather than naming any, so it adds nothing.
/// - Reserved: every range the caller passes, which is how a shim keeps its
///   own image and the tree itself out of the allocator's hands.
///
/// What physload does with the map decides its shape (kernel/phys/memory.cpp,
/// register_range(), into bootmem::PhysicalMemory):
/// - Ranges are neither sorted nor merged. A Ram range goes to add_usable()
///   and a Reserved one to reserve(), which keeps its own list: neither cares
///   about the other's order, a reservation may lie outside RAM, and
///   reservations that overlap are merged there because they are all the same
///   type (FirmwareReserved).
/// - An entry of length zero claims nothing and is left out, because BootMem
///   refuses a zero-length range and physload panics on any range it refuses.
///   An entry that runs past the top of the 64-bit address space is refused
///   here for the same reason.
///
/// The initrd is not in the map, and nothing Reserved in it touches the
/// initrd's pages: physload reserves the boot package itself, in whole pages
/// of `page_size`, as BootPackage, and BootMem refuses a reservation over a
/// range that one of another type holds -- physload would panic. So a
/// reservation block entry, where a bootloader may list the initrd it loaded,
/// is cut around those pages, while a /reserved-memory region or a caller's
/// range overlapping them is refused: two owners claim the same memory, and
/// no choice between them is safe.
///
/// Every member is inline, for the reason legacy_memory.hpp gives: this lib
/// is header-only so a host toolchain can build it.
class DevicetreeTranslator final {
  public:
    /// Rewrite `tree` into ranges(), initrd() and cmdline(), with `reserved`
    /// added to the map as Reserved and `page_size` the granule physload
    /// reserves the initrd in. A second convert() starts over.
    ///
    /// Ranges past MAX_DEVICETREE_MEMORY_RANGES are counted, not written. A
    /// drop is worse here than in a Multiboot2 map: a Reserved range lies
    /// inside RAM, so dropping one hands memory the firmware owns to the
    /// allocator as free. A caller must not pass on a map that dropped
    /// anything.
    ///
    /// Fails, leaving nothing converted, with
    /// - InvArgs: a property not in the shape its binding gives it (a `reg`
    ///   that is not a whole number of entries, an initrd bound that is
    ///   neither one cell nor two, one bound without the other, an end below
    ///   the start, a `bootargs` that is not one string), or a `page_size`
    ///   that is not a power of two;
    /// - NotSupported: a cell width above two (fdt::Node::address_cells());
    /// - OutOfRange: a range, or the initrd's pages, running past 2^64;
    /// - NotFound: no RAM at all, which leaves physload nothing to allocate;
    /// - AlreadyExists: a /reserved-memory region or a caller's range
    ///   overlapping the initrd's pages.
    [[nodiscard]]
    auto convert(fdt::Tree const &tree, std::span<PhysRange const> const reserved,
                 u64 const page_size) -> ustl::Result<Conversion, Status> {
        reset();
        auto const status = translate(tree, reserved, page_size);
        if (status != Status::Ok) {
            reset();
            return ustl::err(status);
        }
        auto const converted = std::min(m_described, MAX_DEVICETREE_MEMORY_RANGES);
        return ustl::ok(Conversion{.converted = converted, .dropped = m_described - converted});
    }

    /// The converted ranges: Conversion::converted of them.
    [[nodiscard]]
    auto ranges() const -> std::span<obi::MemoryRange const> {
        return {m_ranges.data(), std::min(m_described, MAX_DEVICETREE_MEMORY_RANGES)};
    }

    /// Where the firmware loaded the initrd, from /chosen's
    /// `linux,initrd-start` and `linux,initrd-end`; none if it loaded none.
    [[nodiscard]]
    auto initrd() const -> ustl::Option<PhysRange> {
        return m_initrd;
    }

    /// /chosen's `bootargs`, empty if there is none. It points into the tree,
    /// which therefore has to stay where it is while this is read.
    [[nodiscard]]
    auto cmdline() const -> std::string_view {
        return m_cmdline;
    }

  private:
    /// [begin, end) of physical memory: a range already checked to fit.
    struct Interval final {
        u64 begin;
        u64 end;

        [[nodiscard]]
        auto overlaps(Interval const other) const -> bool {
            return begin < other.end && other.begin < end;
        }
    };

    auto reset() -> void {
        m_described = 0;
        m_initrd = ustl::NONE;
        m_cmdline = {};
    }

    [[nodiscard]]
    auto translate(fdt::Tree const &tree, std::span<PhysRange const> const reserved,
                   u64 const page_size) -> Status {
        if (page_size == 0 || (page_size & (page_size - 1)) != 0) {
            return Status::InvArgs;
        }
        if (auto const status = read_chosen(tree); status != Status::Ok) {
            return status;
        }
        auto const initrd = initrd_pages(page_size);
        if (initrd.is_err()) {
            return initrd.unwrap_err();
        }

        if (auto const status = add_memory_nodes(tree.root()); status != Status::Ok) {
            return status;
        }
        if (m_described == 0) {
            return Status::NotFound;
        }
        if (auto const status = add_reservation_block(tree, initrd.unwrap());
            status != Status::Ok) {
            return status;
        }
        if (auto const status = add_reserved_memory(tree, initrd.unwrap()); status != Status::Ok) {
            return status;
        }
        for (auto const range: reserved) {
            if (auto const status = add_owned(range.base, range.size, initrd.unwrap());
                status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto read_chosen(fdt::Tree const &tree) -> Status {
        auto const chosen = tree.root().child("chosen");
        if (chosen.is_none()) {
            return Status::Ok;
        }

        auto const start = chosen.value().property("linux,initrd-start");
        auto const end = chosen.value().property("linux,initrd-end");
        if (start.is_some() != end.is_some()) {
            return Status::InvArgs;
        }
        if (start.is_some()) {
            auto const first = initrd_bound(start.value());
            if (first.is_err()) {
                return first.unwrap_err();
            }
            auto const last = initrd_bound(end.value());
            if (last.is_err()) {
                return last.unwrap_err();
            }
            if (last.unwrap() < first.unwrap()) {
                return Status::InvArgs;
            }
            // An empty initrd is no initrd: there is nothing in it to boot.
            if (last.unwrap() > first.unwrap()) {
                m_initrd =
                    PhysRange{.base = first.unwrap(), .size = last.unwrap() - first.unwrap()};
            }
        }

        auto const bootargs = chosen.value().property("bootargs");
        if (bootargs.is_some()) {
            auto const text = bootargs.value().as_string();
            if (text.is_err()) {
                return text.unwrap_err();
            }
            m_cmdline = text.unwrap();
        }
        return Status::Ok;
    }

    /// A bound of the initrd. The binding lets a bootloader write it as one
    /// cell or two, and both are in use, so both are read.
    [[nodiscard]]
    static auto initrd_bound(fdt::Property const &property) -> ustl::Result<u64, Status> {
        auto const size = property.bytes().size();
        if (size != sizeof(u32) && size != sizeof(u64)) {
            return ustl::err(Status::InvArgs);
        }
        return property.cells(0, property.cell_count());
    }

    /// The pages physload will reserve for the initrd: its range rounded out
    /// to `page_size`. None without an initrd.
    [[nodiscard]]
    auto initrd_pages(u64 const page_size) const -> ustl::Result<ustl::Option<Interval>, Status> {
        if (m_initrd.is_none()) {
            return ustl::ok(ustl::Option<Interval>());
        }
        auto const base = m_initrd.value().base;
        auto const size = m_initrd.value().size;
        auto const mask = page_size - 1;
        if (base > std::numeric_limits<u64>::max() - size ||
            base + size > std::numeric_limits<u64>::max() - mask) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(ustl::Option<Interval>(
            Interval{.begin = base & ~mask, .end = (base + size + mask) & ~mask}));
    }

    [[nodiscard]]
    static auto is_memory(fdt::Node const &node) -> bool {
        auto const type = node.property("device_type");
        if (type.is_none()) {
            return false;
        }
        auto const text = type.value().as_string();
        return text.is_ok() && text.unwrap() == "memory";
    }

    [[nodiscard]]
    auto add_memory_nodes(fdt::Node const &root) -> Status {
        auto const address_cells = root.address_cells();
        if (address_cells.is_err()) {
            return address_cells.unwrap_err();
        }
        auto const size_cells = root.size_cells();
        if (size_cells.is_err()) {
            return size_cells.unwrap_err();
        }

        auto children = root.children();
        for (auto child = children.next(); child.is_some(); child = children.next()) {
            if (!is_memory(child.value())) {
                continue;
            }
            auto const status =
                for_each_reg(child.value(), address_cells.unwrap(), size_cells.unwrap(),
                             [this](fdt::RegEntry const entry) -> Status {
                                 return add(entry.address, entry.size, obi::MemoryRangeType::Ram);
                             });
            if (status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    /// The reservation block, less the initrd's pages: a bootloader that
    /// loaded an initrd may list it here, and physload reserves it under its
    /// own type.
    [[nodiscard]]
    auto add_reservation_block(fdt::Tree const &tree, ustl::Option<Interval> const initrd)
        -> Status {
        for (usize index = 0; index < tree.reservation_count(); ++index) {
            auto const entry = tree.reservation(index).value();
            auto const range = checked(entry.address, entry.size);
            if (range.is_err()) {
                return range.unwrap_err();
            }
            if (range.unwrap().is_none()) {
                continue;
            }

            auto const whole = range.unwrap().value();
            if (initrd.is_none() || !whole.overlaps(initrd.value())) {
                append(whole, obi::MemoryRangeType::Reserved);
                continue;
            }
            if (whole.begin < initrd.value().begin) {
                append({.begin = whole.begin, .end = initrd.value().begin},
                       obi::MemoryRangeType::Reserved);
            }
            if (initrd.value().end < whole.end) {
                append({.begin = initrd.value().end, .end = whole.end},
                       obi::MemoryRangeType::Reserved);
            }
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto add_reserved_memory(fdt::Tree const &tree, ustl::Option<Interval> const initrd) -> Status {
        auto const parent = tree.root().child("reserved-memory");
        if (parent.is_none()) {
            return Status::Ok;
        }
        auto const address_cells = parent.value().address_cells();
        if (address_cells.is_err()) {
            return address_cells.unwrap_err();
        }
        auto const size_cells = parent.value().size_cells();
        if (size_cells.is_err()) {
            return size_cells.unwrap_err();
        }

        auto children = parent.value().children();
        for (auto child = children.next(); child.is_some(); child = children.next()) {
            auto const status =
                for_each_reg(child.value(), address_cells.unwrap(), size_cells.unwrap(),
                             [this, initrd](fdt::RegEntry const entry) -> Status {
                                 return add_owned(entry.address, entry.size, initrd);
                             });
            if (status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    /// A Reserved range someone owns for good -- the firmware or the shim --
    /// which therefore must not share a page with the initrd.
    [[nodiscard]]
    auto add_owned(u64 const base, u64 const size, ustl::Option<Interval> const initrd) -> Status {
        auto const range = checked(base, size);
        if (range.is_err()) {
            return range.unwrap_err();
        }
        if (range.unwrap().is_none()) {
            return Status::Ok;
        }
        if (initrd.is_some() && range.unwrap().value().overlaps(initrd.value())) {
            return Status::AlreadyExists;
        }
        append(range.unwrap().value(), obi::MemoryRangeType::Reserved);
        return Status::Ok;
    }

    [[nodiscard]]
    auto add(u64 const base, u64 const size, obi::MemoryRangeType const type) -> Status {
        auto const range = checked(base, size);
        if (range.is_err()) {
            return range.unwrap_err();
        }
        if (range.unwrap().is_some()) {
            append(range.unwrap().value(), type);
        }
        return Status::Ok;
    }

    /// [base, base + size), none when it is empty, or OutOfRange when its end
    /// does not fit in 64 bits (BootMem refuses a range ending at 2^64 too).
    [[nodiscard]]
    static auto checked(u64 const base, u64 const size)
        -> ustl::Result<ustl::Option<Interval>, Status> {
        if (size == 0) {
            return ustl::ok(ustl::Option<Interval>());
        }
        if (base > std::numeric_limits<u64>::max() - size) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(ustl::Option<Interval>(Interval{.begin = base, .end = base + size}));
    }

    /// Every entry of `node`'s `reg`, decoded with the given cell widths (its
    /// parent's), passed to `visit` until it returns something other than Ok.
    template <typename Visit>
    [[nodiscard]]
    static auto for_each_reg(fdt::Node const &node, usize const address_cells,
                             usize const size_cells, Visit &&visit) -> Status {
        auto const reg = node.property("reg");
        if (reg.is_none()) {
            return Status::Ok;
        }
        if (address_cells == 0) {
            return Status::InvArgs;
        }
        auto const stride = (address_cells + size_cells) * fdt::Property::CELL_SIZE;
        auto const bytes = reg.value().bytes().size();
        if (bytes % stride != 0) {
            return Status::InvArgs;
        }
        for (usize index = 0; index < bytes / stride; ++index) {
            auto const entry = reg.value().reg(address_cells, size_cells, index);
            if (entry.is_err()) {
                return entry.unwrap_err();
            }
            if (auto const status = visit(entry.unwrap()); status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    /// Write the range if there is room and count it either way, so that
    /// convert() can say how many were described.
    auto append(Interval const range, obi::MemoryRangeType const type) -> void {
        if (m_described < MAX_DEVICETREE_MEMORY_RANGES) {
            m_ranges[m_described] = obi::MemoryRange{
                .base = range.begin,
                .length = range.end - range.begin,
                .type = static_cast<u32>(type),
                .reserved = 0,
            };
        }
        ++m_described;
    }

    DevicetreeMemoryRanges m_ranges{};

    /// Ranges the tree and the caller described, written or not.
    usize m_described{0};

    ustl::Option<PhysRange> m_initrd{};
    std::string_view m_cmdline{};
};

} // namespace bootshim

#endif // BOOTSHIM_DEVICETREE_HPP
