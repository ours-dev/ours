/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTMEM_REGION_HPP
#define BOOTMEM_REGION_HPP 1

#include <bootmem/types.hpp>

#include <ustl/bit.hpp>
#include <ustl/bitfields.hpp>
#include <ustl/mem/align.hpp>

namespace bootmem {
    struct Region {
        Region() = default;

        Region(PhysAddr base, PhysAddr size, RegionType type, NnId nid)
            : base(base),
              size(size),
              flags() {
            set_nid(nid);
            set_type(type);
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto trim(usize align) -> void {
            auto const x = ustl::mem::align_up(base, align);
            size = ustl::mem::align_down(size - (x - base), align);
            base = x;
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto nid() const -> isize {
            return flags.nid();
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto set_nid(NnId nid) -> void {
            flags.set_nid(nid);
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto type() const -> RegionType {
            return flags.type();
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto set_type(RegionType type) -> void {
            flags.set_type(type);
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto end() const -> PhysAddr {
            return base + size;
        }

        PhysAddr base;
        PhysAddr size;
        RegionFlags flags;
    };

    template <typename Region, bool Negative>
    struct RegionTraits {
        FORCE_INLINE CXX11_CONSTEXPR
        static auto base(Region const &region) -> PhysAddr {
            return region.base;
        }

        FORCE_INLINE CXX11_CONSTEXPR
        static auto size(Region const &region) -> PhysAddr {
            return region.size;
        }

        FORCE_INLINE CXX11_CONSTEXPR
        static auto end(Region const &region) -> PhysAddr {
            return base(region) + size(region);
        }
    };

    template <typename Region>
    struct RegionTraits<Region, true> {
        FORCE_INLINE CXX11_CONSTEXPR
        static auto base(Region const &region) -> PhysAddr {
            return region.base + region.size;
        }

        FORCE_INLINE CXX11_CONSTEXPR
        static auto size(Region const &region) -> PhysAddr {
            return region.size;
        }

        FORCE_INLINE CXX11_CONSTEXPR
        static auto end(Region const &region) -> PhysAddr {
            return region.base;
        }
    };

} // namespace bootmem

#endif // #ifndef BOOTMEM_REGION_HPP