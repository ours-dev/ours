/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ours/mm/physmap.hpp>

#include <concepts>
#include <type_traits>

#include <cstdlib>

#include <ustl/fmt/make_format_args.hpp>

namespace {
using ours::PhysAddr;
using ours::VirtAddr;
using ours::mm::PhysMap;

template <typename Left, typename Right>
concept Addable = requires(Left left, Right right) { left + right; };

template <typename Left, typename Right>
concept Subtractable = requires(Left left, Right right) { left - right; };

static_assert(std::same_as<decltype(0_va), VirtAddr>);
static_assert(std::same_as<decltype(0_pa), PhysAddr>);
static_assert(0_va == VirtAddr{} && 0_pa == PhysAddr{});
static_assert(0x1234_va == VirtAddr{0x1234} && 4096_pa == PhysAddr{4096});
static_assert(0x8000'0000'0000'0000_pa == PhysAddr{1_usize << 63});
static_assert(0xffff'ffff'ffff'ffff_va == VirtAddr::Invalid);

static_assert(!std::is_convertible_v<VirtAddr, usize>);
static_assert(!std::is_convertible_v<usize, VirtAddr>);
static_assert(!std::is_convertible_v<PhysAddr, usize>);
static_assert(!std::is_convertible_v<usize, PhysAddr>);
static_assert(!std::is_convertible_v<VirtAddr, PhysAddr>);
static_assert(!std::is_convertible_v<PhysAddr, VirtAddr>);
static_assert(!Addable<VirtAddr, PhysAddr> && !Addable<PhysAddr, VirtAddr>);
static_assert(!Addable<VirtAddr, VirtAddr> && !Addable<PhysAddr, PhysAddr>);
static_assert(!Subtractable<VirtAddr, PhysAddr> && !Subtractable<PhysAddr, VirtAddr>);
static_assert(std::same_as<decltype(VirtAddr{} + usize{}), VirtAddr>);
static_assert(std::same_as<decltype(PhysAddr{} + usize{}), PhysAddr>);
static_assert(std::same_as<decltype(VirtAddr{} - VirtAddr{}), usize>);
static_assert(std::same_as<decltype(PhysAddr{} - PhysAddr{}), usize>);
static_assert(sizeof(VirtAddr) == sizeof(usize) && sizeof(PhysAddr) == sizeof(usize));
static_assert(std::is_trivially_copyable_v<VirtAddr> && std::is_trivially_copyable_v<PhysAddr>);
static_assert(VirtAddr::Anywhere >= ours::VIRT_ADDR_RESERVED_BASE);
static_assert(VirtAddr::Invalid >= ours::VIRT_ADDR_RESERVED_BASE);
static_assert(VirtAddr::Anywhere != VirtAddr::Invalid &&
              VirtAddr{} < ours::VIRT_ADDR_RESERVED_BASE);

// Physical zero remains translatable; failure has a different representation.
static_assert(PhysMap::phys_to_virt(PhysMap::PHYS_BASE) == PhysMap::VIRT_BASE);
static_assert(PhysMap::virt_to_phys(PhysMap::VIRT_BASE) == PhysMap::PHYS_BASE);
static_assert(PhysMap::phys_to_virt(PhysMap::PHYS_BASE + PhysMap::SIZE) == VirtAddr::Invalid);
static_assert(PhysMap::virt_to_phys(PhysMap::VIRT_BASE + PhysMap::SIZE) == PhysAddr::Invalid);
static_assert(ustl::round_down(VirtAddr{0x1234}, 0x1000) == VirtAddr{0x1000});
static_assert(ustl::round_up(PhysAddr{0x1234}, 0x1000) == PhysAddr{0x2000});
static_assert(ustl::is_aligned(PhysAddr{}, 0x1000));
} // namespace

auto main() -> int {
    if (PhysMap::phys_to_virt<u8>(PhysAddr::Invalid) != nullptr ||
        std::format("{:#x}", ustl::fmt::decay_format_arg(VirtAddr{0x1234})) != "0x1234" ||
        std::format("{:#x}", ustl::fmt::decay_format_arg(PhysAddr{0x5678})) != "0x5678") {
        std::abort();
    }
    return 0;
}
