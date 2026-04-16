/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_PACKAGE_HPP
#define OURS_PHYS_KERNEL_PACKAGE_HPP 1

#include <ours/types.hpp>
#include <ours/compiler.hpp>
#include <ours/status.hpp>

#include <arch/tick.hpp>
#include <obi/obi-view.hpp>

namespace ours::phys {
    /// It ownes a group of OBI which contain `main.image`, `initrd` and e.g.
    struct KernelPackage {
        typedef obi::ObiView  Obi;
        typedef Obi::Iterator   Item;

        auto init(Obi const &obi) -> Status;

        FORCE_INLINE CXX11_CONSTEXPR
        auto init(PhysAddr obi) -> Status {
            return init(Obi::from(reinterpret_cast<obi::ObiHeader *>(obi)));
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto crate() const -> Item {
            return package_;
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto package() const -> ustl::views::Span<u8> {
            return package_loaded_;
        }
    private:
        auto unpack() -> Status;

        Obi obi_;
        Item package_;
        ustl::views::Span<u8> package_loaded_;
        arch::Tick package_load_start_;
        arch::Tick package_load_end_;
    };

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_KERNEL_PACKAGE_HPP