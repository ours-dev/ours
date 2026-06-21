/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_GAF_HPP
#define OURS_MEM_GAF_HPP

#include <ours/mem/types.hpp>
#include <ours/macro_abi.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::mem {
namespace priv {
    /// Shorthand of Getting Available Frames.
    enum class Gaf {
        /// Physical address zone modifiers.
        Dma         = 0_bit,
        Dma32       = 1_bit,
        Movable     = 2_bit,

        /// `PMM` will retry infinitely until it get an available frame.
        NoFail          = 3_bit,
        NoRetry         = 4_bit,

        /// Indicates the caller intends to dirty the page.
        Write           = 5_bit,

        /// In process of an allocation, it indicates `PMM` under memory pressure
        /// that it could enable the back-side thread to reclaim frames and sleep
        /// applicant's thread to avoid wasting CPU time.
        DaemonReclaim   = 6_bit,

        /// Allow `PMM` to directly reclaim frames under certain memory pressure,
        /// meaning that the process of reclaim is linear so the applicant does not
        /// wait through sleeping.
        DirectReclaim   = 7_bit,

        Compound        = 8_bit,

        ThisNode        = 9_bit,

        /// Return a zeroed frame on success.
        Zero            = 10_bit,

        Reclaim = DirectReclaim | DaemonReclaim,
    };
    USTL_ENABLE_ENUM_BITMASK(Gaf);

    FORCE_INLINE CXX11_CONSTEXPR
    auto gaf_zone_type(Gaf gaf) -> ZoneType {
        using Underlying = ustl::traits::UnderlyingTypeT<Gaf>;

        // ZONE_SHIFT 
        CXX11_CONSTEXPR
        auto const zone_mask = Gaf::Dma | Gaf::Dma32 | Gaf::Movable;

        CXX11_CONSTEXPR
        auto const make_entry = [] (ZoneType type, Gaf group) {
            return static_cast<Underlying>(type) << static_cast<Underlying>(group) * ZONES_SHIFT;
        };

        CXX11_CONSTEXPR
        auto const zone_map =
            // By default, use ZoneType::Normal
            make_entry(ZoneType::Normal, Gaf(0)) |
            make_entry(ZoneType::Dma, Gaf::Dma) | 
            make_entry(ZoneType::Dma32, Gaf::Dma32) | 
            make_entry(ZoneType::Normal, Gaf::Movable) | 
            make_entry(ZoneType::Dma, Gaf::Dma | Gaf::Movable) | 
            make_entry(ZoneType::Dma32, Gaf::Dma32 | Gaf::Movable);

        // Extract the bits that indicates the type of zone.
        auto const bits = Underlying(gaf & zone_mask);
        auto zone = (zone_map >> (Underlying(bits) * ZONES_SHIFT)) & ((1 << ZONES_SHIFT) - 1);
        return ZoneType(zone);
    }
    static_assert(gaf_zone_type(Gaf::Reclaim) == ZoneType::Normal);
    static_assert(gaf_zone_type(Gaf::Dma) == ZoneType::Dma);
    static_assert(gaf_zone_type(Gaf::Dma32) == ZoneType::Dma32);

    CXX11_CONSTEXPR
    static auto const kGafBoot = Gaf::ThisNode | Gaf::Zero;

    CXX11_CONSTEXPR
    static auto const kGafKernel = Gaf::ThisNode | Gaf::Reclaim;

    CXX11_CONSTEXPR
    static auto const kGafUser= Gaf::ThisNode;

    enum class AllocationControl: u32 {
        NoFragment = 0_bit,

        NoBlock = 1_bit,

        Contiguous = 2_bit,

        Kswapd = 3_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(AllocationControl);

} // namespace priv
    using priv::Gaf;
    using priv::AllocationControl;
    using priv::kGafKernel;
    using priv::kGafBoot;
    using priv::kGafUser;

} // namespace ours::mem

#endif // OURS_MEM_GAF_HPP
