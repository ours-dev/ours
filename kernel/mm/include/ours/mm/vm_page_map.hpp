/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_PAGE_MAP_HPP
#define OURS_MM_VM_PAGE_MAP_HPP 1

#include <ours/const.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/vm_page.hpp>

#include <ktl/new.hpp>
#include <ktl/allocator.hpp>
#include <ktl/xarray.hpp>
#include <ustl/bitfields.hpp>

namespace ours::mm {
    class VmPageMap {
        using Self = VmPageMap;
      public:
        enum class Tag {
            Owned,
            Borrowed = BIT(0),
        };

        FORCE_INLINE
        auto get_page(PgOff index) -> VmPage * {
            auto option = pages_.load(index).cast_to<VmPage *>();
            if (!option) {
                return nullptr;
            }
            return *option;
        }

        FORCE_INLINE
        auto insert_page(PgOff index, VmPage *page, Tag tag) -> void {
            pages_.store(index, pages_.make_entry(page));
        }
      private:
        ktl::XArray<ktl::Allocator<char>> pages_;
    };

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_PAGE_MAP_HPP