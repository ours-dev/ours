/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTSHIM_LEGACY_MEMORY_HPP
#define BOOTSHIM_LEGACY_MEMORY_HPP 1

#include <bootshim/conversion.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include <multiboot/view.hpp>
#include <obi/package_format.hpp>
#include <ours/types.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/option.hpp>

/// What a boot-shim does with a legacy platform's memory map, separated from
/// the shim that does it.
///
/// A boot-shim's job is to translate one bootloader protocol into the
/// vocabulary everything downstream of it already speaks, and the memory map
/// is the part of that translation with actual decisions in it: which
/// firmware range types count as usable RAM, what to do when there are more
/// ranges than a boot image can hold without an allocator, and where in the
/// map there is room to place the next image.  None of that needs a running
/// machine to exercise, so it lives here rather than inside
/// kernel/arch/x86/phys/bootshim/shim.cpp, where it could only ever be
/// tested by booting.
///
/// Nothing here prints or panics: a shim decides how loud a truncated memory
/// map or a failed placement should be, and a host test has no console to be
/// loud on.  Both functions report what happened and let the caller react.
namespace bootshim {
using ours::PhysAddr;

using MemoryMap = multiboot::View::MemoryMap;

/// A boot-shim never expects more than a handful of firmware-reported
/// ranges; if a platform ever hands it more, better to say so loudly
/// (missing ranges silently become "not usable") than to allocate for an
/// unbounded count this early -- there is no allocator yet at the point a
/// shim reads this map.
inline constexpr auto MAX_LEGACY_MEMORY_RANGES = usize{64};

using LegacyMemoryRanges = std::array<obi::MemoryRange, MAX_LEGACY_MEMORY_RANGES>;

/// Owns one legacy memory map's rewrite into the same obi::MemoryRange
/// vocabulary a real OBI memory item already carries, so that whether these
/// ranges arrived as an OBI item or through a shim's side channel changes
/// only how they were delivered, not their shape.
///
/// Every member is inline: lib/bootshim is a deliberate header-only seam
/// (the host toolchain cannot evaluate //kernel/phys targets, so this lib
/// must not depend on any of them), and an out-of-line member would break
/// it.
class LegacyMemoryTranslator final {
  public:
    /// Rewrite `map` into m_ranges, in map order -- the firmware's, not
    /// sorted by usefulness, so a truncated tail can perfectly well be RAM
    /// -- stopping at MAX_LEGACY_MEMORY_RANGES; the ranges past that point
    /// are counted, not written.  A second convert() on the same object
    /// resets it first, so one instance serves a retry cleanly.
    ///
    /// A drop costs memory, never correctness: the firmware's ranges do not
    /// overlap, so a dropped range is only ever left out of usable RAM.
    [[nodiscard]]
    auto convert(MemoryMap const &map) -> Conversion {
        m_converted = 0;
        auto const total = map.count();
        m_converted = std::min(total, MAX_LEGACY_MEMORY_RANGES);
        for (usize index = 0; index < m_converted; ++index) {
            auto const entry = map.range(index);
            m_ranges[index] = obi::MemoryRange{
                .base = entry.addr,
                .length = entry.len,
                .type = static_cast<u32>(to_obi_type(entry.type)),
                .reserved = 0,
            };
        }
        return {.converted = m_converted, .dropped = total - m_converted};
    }

    /// The converted prefix -- size() always equals the Conversion::converted
    /// of the last convert() call.
    [[nodiscard]]
    auto ranges() const -> std::span<obi::MemoryRange const> {
        return {m_ranges.data(), m_converted};
    }

    /// Find `alignment`-aligned room for `size` bytes of Available memory at
    /// or after `floor`.
    ///
    /// A bump allocator over the platform's own memory map, not over any
    /// image's idea of memory: at the point a shim needs this, the phys
    /// runtime that owns BootMem has not been loaded yet, and nothing
    /// upstream of the shim owns memory management either.
    ///
    /// `floor` is the caller's whole means of protecting anything: this
    /// function only guarantees the returned range starts at or after it,
    /// never that nothing the caller cares about sits inside the range it
    /// hands back.  A caller with several things to avoid therefore has to
    /// pass the highest end among them and give up the memory below -- which
    /// is safe but wasteful, and the reason this signature is worth
    /// revisiting once there is a real reservation list to consult.
    ///
    /// Returns NONE when no Available region has room, including when
    /// `alignment` is not a power of two -- an aligned address cannot be
    /// meaningfully computed then, and silently rounding to something else
    /// would hand back a range the caller's own alignment assumptions do not
    /// hold for.
    [[nodiscard]]
    static auto find_scratch(MemoryMap const &map, PhysAddr const floor, usize const size,
                             usize const alignment) -> ustl::Option<PhysAddr> {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return ustl::NONE;
        }
        for (usize index = 0; index < map.count(); ++index) {
            auto const region = map.range(index);
            if (region.type != multiboot::MultibootMmapEntry::Available || region.len < size) {
                continue;
            }
            if (region.addr > std::numeric_limits<u64>::max() - region.len) {
                continue; // Malformed entry; already rejected by MemoryMap::from in
                          // principle, but this function does not get to assume that.
            }
            auto const region_end = region.addr + region.len;
            auto const candidate = ustl::round_up(std::max(region.addr, static_cast<u64>(floor)),
                                                  static_cast<u64>(alignment));
            if (candidate < region.addr || candidate >= region_end ||
                region_end - candidate < size) {
                continue;
            }
            return ustl::some(static_cast<PhysAddr>(candidate));
        }
        return ustl::NONE;
    }

    /// Multiboot2 distinguishes several flavours of unusable (reserved, ACPI
    /// reclaimable, NVS, bad RAM); OBI's early memory vocabulary does not, and
    /// the consumer -- BootMem, through arch_detect_memory() -- only ever asks
    /// "may I allocate here".  Anything that is not plain Available answers no,
    /// so it all folds to Reserved.
    [[nodiscard]]
    static constexpr auto to_obi_type(u32 const multiboot_type) noexcept -> obi::MemoryRangeType {
        return multiboot_type == multiboot::MultibootMmapEntry::Available
                   ? obi::MemoryRangeType::Ram
                   : obi::MemoryRangeType::Reserved;
    }

  private:
    LegacyMemoryRanges m_ranges{};
    usize m_converted{0};
};

} // namespace bootshim

#endif // BOOTSHIM_LEGACY_MEMORY_HPP
