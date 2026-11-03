/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/platform/mmio.hpp>

#include <array>
#include <span>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::platform {
namespace {
    /// How many devices a platform can name as the kernel's own. The PC has a
    /// local APIC, an IO-APIC per 24 lines the firmware lists, and an HPET.
    /// More than this is not a reason to stop the boot: it refuses every
    /// request instead, see KERNEL_MMIO_FULL.
    constexpr auto MAX_KERNEL_MMIO = usize{32};

    /// How many runs of RAM the snapshot holds. It mirrors the capacity of the
    /// map it copies (MAX_EARLY_MEMORY_REGIONS in kernel/mm/init_pmm.cpp), so
    /// the snapshot never has fewer slots than that map has regions.
    constexpr auto MAX_KERNEL_RAM = usize{128};

    /// Written only by reserve_kernel_mmio(), which runs while the platform
    /// initialises, one call after another on the boot CPU and before any user
    /// program exists; read afterwards by whoever asks. Nothing writes it once
    /// the first program runs, so it takes no lock.
    std::array<MmioRange, MAX_KERNEL_MMIO> KERNEL_MMIO;
    usize KERNEL_MMIO_COUNT = 0;

    /// Set when the platform named more devices than KERNEL_MMIO holds. The
    /// ones past the end are not recorded, so nothing can say which addresses
    /// are safe any more: touches_kernel_physical() answers yes to everything.
    bool KERNEL_MMIO_FULL = false;

    /// The RAM the memory manager knew at boot, in whole pages, copied by
    /// snapshot_ram() before the memory its map lives in is given back.
    ///
    /// That map (mm::EarlyMem) is in the init sections, and the page allocator
    /// owns those frames from the end of boot: a reader a moment later would
    /// see whatever a new owner wrote there, and an all-zero map reads as "no
    /// RAM anywhere". These arrays are ordinary static storage, which stays.
    /// Written once by snapshot_ram() on the boot CPU, before any user program
    /// exists, and only read afterwards.
    std::array<MmioRange, MAX_KERNEL_RAM> KERNEL_RAM;
    usize KERNEL_RAM_COUNT = 0;

    /// Whether snapshot_ram() copied the whole map. Until it has, and if it
    /// could not, nothing is known not to be RAM: touches_ram() answers yes.
    bool KERNEL_RAM_KNOWN = false;

    /// Whether [base, base + size) and [other_base, other_base + other_size)
    /// share an address. Neither run may be empty, nor end past the 64-bit
    /// space.
    auto overlaps(PhysAddr const base, u64 const size, PhysAddr const other_base,
                  u64 const other_size) -> bool {
        return base < other_base + other_size && other_base < base + size;
    }

    /// Whether any RAM the kernel knows of lies in [base, base + size), claimed
    /// or free.
    auto touches_ram(PhysAddr const base, u64 const size) -> bool {
        if (!KERNEL_RAM_KNOWN) {
            return true;
        }
        for (auto const &range: kernel_ram()) {
            if (overlaps(base, size, range.base, range.size)) {
                return true;
            }
        }
        return false;
    }

    /// Copies the memory manager's map of usable RAM into KERNEL_RAM, each run
    /// grown outward to whole pages: a mapping covers whole pages, so the page
    /// holding the edge of a run is RAM to a program that maps it, whatever the
    /// map trimmed it to.
    ///
    /// Runs as an init hook, after the map exists and well before
    /// EarlyMem::reclaim_init_area() gives it back.
    INIT_CODE
    auto snapshot_ram() -> void {
        auto count = usize{0};
        auto fits = true;
        auto const status = mm::EarlyMem::visit_usable(
            mm::EarlyMem::min_address(), mm::EarlyMem::max_address(), bootmem::ANY_NODE,
            [&](bootmem::MemoryRegion const &region) {
                if (count == KERNEL_RAM.size()) {
                    fits = false;
                    return false;
                }
                auto const first = ustl::round_down(region.range.base, u64{PAGE_SIZE});
                auto const last = ustl::round_up(region.range.end(), u64{PAGE_SIZE});
                KERNEL_RAM[count] = MmioRange{.base = first, .size = last - first};
                count += 1;
                return true;
            });
        if (status != Status::Ok || !fits) {
            log::error("platform: the RAM map was not copied ({}, {} runs), so every physical "
                       "address counts as RAM",
                       u32(status), count);
            return;
        }

        KERNEL_RAM_COUNT = count;
        KERNEL_RAM_KNOWN = true;
    }
} // namespace

GKTL_INIT_HOOK(KernelRamSnapshot, snapshot_ram, gktl::InitLevel::Vmm);

auto reserve_kernel_mmio(PhysAddr const base, u64 const size) -> void {
    ASSERT(size != 0, "the kernel reserved an empty run of registers at {:#x}", base);
    if (KERNEL_MMIO_COUNT == MAX_KERNEL_MMIO) {
        KERNEL_MMIO_FULL = true;
        log::error("platform: the kernel names more than {} devices of its own, so every "
                   "physical address counts as one",
                   MAX_KERNEL_MMIO);
        return;
    }

    auto const first = ustl::round_down(base, u64{PAGE_SIZE});
    auto const last = ustl::round_up(base + size, u64{PAGE_SIZE});
    KERNEL_MMIO[KERNEL_MMIO_COUNT] = MmioRange{.base = first, .size = last - first};
    KERNEL_MMIO_COUNT += 1;
}

auto kernel_mmio() -> ustl::views::Span<MmioRange const> {
    return ustl::views::Span<MmioRange const>(KERNEL_MMIO.data(), KERNEL_MMIO_COUNT);
}

auto kernel_ram() -> ustl::views::Span<MmioRange const> {
    return ustl::views::Span<MmioRange const>(KERNEL_RAM.data(), KERNEL_RAM_COUNT);
}

auto touches_kernel_physical(PhysAddr const base, u64 const size) -> bool {
    // A request that covers no address shares none.
    if (size == 0) {
        return false;
    }
    if (KERNEL_MMIO_FULL) {
        return true;
    }

    for (auto const &range: kernel_mmio()) {
        if (overlaps(base, size, range.base, range.size)) {
            return true;
        }
    }
    return touches_ram(base, size);
}
} // namespace ours::platform
