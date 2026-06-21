/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_EARLY_HPP
#define OURS_MEM_EARLY_HPP 1

#include <ours/init.hpp>
#include <ours/mem/types.hpp>
#include <ours/mem/physmap.hpp>
#include <ours/mem/cfg.hpp>

#include <ours/phys/handoff.hpp>

#include <ustl/mem/align.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/algorithms/minmax.hpp>

#include <gktl/range.hpp>

#ifdef OURS_CONFIG_MEM_HOTPLUG
#define MEM_INIT_CODE
#define MEM_INIT_DATA
#else
#define MEM_INIT_CODE  INIT_CODE
#define MEM_INIT_DATA  INIT_DATA
#endif

namespace ours::mem {
    struct EarlyMem {
        typedef phys::BootMem::IterationContext     IterationContext;

        static auto dump() -> void;

        FORCE_INLINE
        static auto protect(PhysAddr base, usize size) -> void {  
            s_bootmem->protect(base, size);  
        }

        FORCE_INLINE
        static auto set_node(PhysAddr base, usize size, NnId nid) -> void {  
            s_bootmem->set_node(base, size, nid);  
        }

        template <typename T = u8>
        FORCE_INLINE
        static auto allocate(usize n, NnId nid) -> T * {  
            return allocate<T>(n, usize(alignof(T)), nid);
        }

        template <typename T = u8>
        FORCE_INLINE
        static auto allocate(usize n, usize align, NnId nid) -> T * {
            return allocate<T>(n, align, min_address(), max_address(), nid);
        }

        template <typename T = u8>
        FORCE_INLINE
        static auto allocate(usize n, usize align, PhysAddr start, PhysAddr end, NnId nid) -> T * {
            DEBUG_ASSERT(s_bootmem);
            PhysAddr phys_addr = s_bootmem->allocate(sizeof(T) * n, align, start, end, nid);
            if (!phys_addr) {
                return nullptr;
            }
            return PhysMap::phys_to_virt<T>(phys_addr);
        }

        template <typename T>
        FORCE_INLINE
        static auto deallocate(T *ptr, usize n) -> void {
            DEBUG_ASSERT(s_bootmem);
            PhysAddr phys_addr = PhysMap::virt_to_phys(ptr);
            s_bootmem->deallocate(phys_addr, sizeof(T) * n);
        }

        FORCE_INLINE
        static auto iterate(IterationContext &context) -> ustl::Option<bootmem::Region> {  
            return s_bootmem->iterate(context);  
        }

        static auto get_pfn_range_node(NnId nid) -> gktl::Range<Pfn>;

        static auto count_present_frames(Pfn start, Pfn end) -> usize;

        static auto do_handoff() -> void;

        static auto reclaim_init_area() -> void;

        FORCE_INLINE
        static auto min_address() -> PhysAddr {  
            return s_bootmem->min_address;
        }

        FORCE_INLINE
        static auto max_address() -> PhysAddr {  
            return s_bootmem->max_address;
        }

        INIT_DATA
        static inline phys::EarlyMem *s_bootmem;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_EARLY_HPP