/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_EARLY_HPP
#define OURS_MM_EARLY_HPP 1

#include <ours/init.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/cfg.hpp>

#include <ours/phys/handoff.hpp>

#include <ustl/mem/align.hpp>
#include <gktl/range.hpp>

#include <limits>

#ifdef OURS_CONFIG_MEM_HOTPLUG
#define MEM_INIT_CODE
#define MEM_INIT_DATA
#else
#define MEM_INIT_CODE  INIT_CODE
#define MEM_INIT_DATA  INIT_DATA
#include <algorithm>
#endif

namespace ours::mm {
    struct EarlyMem {
        static auto dump() -> void;

        [[nodiscard]] static auto reserve(PhysAddr const base, usize const size) -> Status {
            auto const range = bootmem::Range::from(base, size);
            if (range.is_err()) {
                return range.unwrap_err();
            }
            return s_bootmem == nullptr ? Status::BadState : s_bootmem->reserve(range.unwrap());
        }

        [[nodiscard]] static auto set_node(PhysAddr const base, usize const size, NnId const node) -> Status {
            auto const range = bootmem::Range::from(base, size);
            if (range.is_err()) {
                return range.unwrap_err();
            }
            return s_bootmem == nullptr ? Status::BadState
                : s_bootmem->assign_node(range.unwrap(), node);
        }

        template <typename T = u8>
        static auto allocate(usize const count, NnId const node) -> T * {
            return allocate<T>(count, alignof(T), min_address(), max_address(), node);
        }

        template <typename T = u8>
        static auto allocate(usize const count, usize const alignment, NnId const node) -> T * {
            return allocate<T>(count, alignment, min_address(), max_address(), node);
        }

        template <typename T = u8>
        static auto allocate(usize const count, usize const alignment, PhysAddr const lower,
                             PhysAddr const upper, NnId const node) -> T * {
            if (s_bootmem == nullptr || count > std::numeric_limits<usize>::max() / sizeof(T)) {
                return nullptr;
            }
            auto const allocation = s_bootmem->allocate({
                .size = sizeof(T) * count,
                .alignment = alignment,
                .lower = lower,
                .upper = upper,
                .node = node,
                .direction = bootmem::AllocationDirection::TopDown,
            });
            return allocation.is_ok() ? PhysMap::phys_to_virt<T>(allocation.unwrap().base) : nullptr;
        }

        template <typename T>
        [[nodiscard]] static auto deallocate(T *const pointer, usize const count) -> Status {
            if (s_bootmem == nullptr || count > std::numeric_limits<usize>::max() / sizeof(T)) {
                return Status::BadState;
            }
            auto const range = bootmem::Range::from(PhysMap::virt_to_phys(pointer), sizeof(T) * count);
            if (range.is_err()) {
                return range.unwrap_err();
            }
            return s_bootmem->release(range.unwrap());
        }

        template <typename Visitor>
        [[nodiscard]] static auto visit(bootmem::MemoryQuery const query, Visitor &&visitor) -> Status {
            if (s_bootmem == nullptr) {
                return Status::BadState;
            }
            return s_bootmem->visit(query, visitor);
        }

        template <typename Visitor>
        [[nodiscard]] static auto visit_usable(PhysAddr const lower, PhysAddr const upper, NnId const node,
                                                Visitor &&visitor) -> Status {
            return visit({
                .lower = lower,
                .upper = upper,
                .node = node,
                .view = bootmem::MemoryView::Usable,
            }, visitor);
        }

        template <typename Visitor>
        [[nodiscard]] static auto visit_free(PhysAddr const lower, PhysAddr const upper, NnId const node,
                                              Visitor &&visitor) -> Status {
            return visit({
                .lower = lower,
                .upper = upper,
                .node = node,
                .view = bootmem::MemoryView::Free,
            }, visitor);
        }

        static auto get_pfn_range_node(NnId nid) -> gktl::Range<Pfn>;

        static auto count_present_frames(Pfn start, Pfn end) -> usize;

        static auto do_handoff() -> void;

        static auto reclaim_init_area() -> void;

        [[nodiscard]] static auto min_address() -> PhysAddr {
            return s_bootmem == nullptr ? PhysAddr {} : s_bootmem->min_address();
        }

        [[nodiscard]] static auto max_address() -> PhysAddr {
            return s_bootmem == nullptr ? PhysAddr {} : s_bootmem->max_address();
        }

        INIT_DATA
        static inline phys::BootMem *s_bootmem;
    };

} // namespace ours::mm

#endif // #ifndef OURS_MM_EARLY_HPP
