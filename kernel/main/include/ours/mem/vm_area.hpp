/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_AREA_HPP
#define OURS_MEM_VM_AREA_HPP 1

#include <ours/status.hpp>
#include <ours/mem/types.hpp>
#include <ours/mem/vm_fault.hpp>
#include <ours/mem/vm_area_or_mapping.hpp>

#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/collections/intrusive/set.hpp>

#include <ktl/result.hpp>
#include <gktl/range.hpp>
#include <gktl/canary.hpp>

namespace ours::mem {
    /// `VmArea` is a representation of a contiguous range of virtual memory space.
    class VmArea final: public VmAreaOrMapping {
        using Self = VmArea;
        using Base = VmAreaOrMapping;
      public:
        FORCE_INLINE
        static auto create(VirtAddr va, usize size, VmaFlags vmaf, VmArea *parent, VmAspace *aspace, 
                           char const *name, ustl::Rc<Self> *out) -> Status {
            DEBUG_ASSERT(parent);
            return create_common(va, size, vmaf, parent, aspace, name, out);
        }

        FORCE_INLINE
        static auto create_root(VirtAddr va, usize size, VmaFlags vmaf, VmAspace *aspace, char const *name, 
                                ustl::Rc<Self> *out) -> Status {
            return create_common(va, size, vmaf, nullptr, aspace, name, out);
        }

        /// Create a mapping unit in page range [vma_off, vma_off + size).
        auto create_mapping(usize vma_ofs, usize size, usize vmo_ofs, MmuFlags mmuf, ustl::Rc<VmObject> vmo, 
                            char const *name, VmMapOption option, ustl::Rc<VmMapping> *out) -> Status;

        /// Create a sub-VMA in page range [vma_off, vma_off + nr_pages).
        auto create_subvma(usize offset, usize size, VmaFlags vmaf, char const *name, VmMapOption option, 
                           ustl::Rc<Self> *out) -> Status;

        /// Following map* interfaces are the a group of coarse-grained method to provide a convenient way for 
        /// mapping a segment of VMA.
        ///
        /// When offer option |VmMapOption::Fixed| the range [base, base + size) would be forcely 
        /// used, namely those existing mappings which overlaps with it will be replaced with expected one.
        ///
        /// When offer option |VmMapOption::Commit|, this mapping request will be committed intermediately, 
        /// rather than wait a page fault to trigger real mapping action. 
        ///
        /// On success them return a object of VmMapping.

        /// Map |size| bytes space from the given physical address |phys_base| to virtual address |virt_base|.
        auto map_at(PhysAddr phys_base, VirtAddr *virt_base, usize size, MmuFlags, VmMapOption, char const *name)
            -> ktl::Result<ustl::Rc<VmMapping>>;

        /// Map |size| bytes space to virtual address |virt_base|.
        auto map(VirtAddr *base, usize size, MmuFlags, VmMapOption, char const * name)
            -> ktl::Result<ustl::Rc<VmMapping>>;

        auto unmap(usize offset, usize size) -> Status;

        /// Update rights of the subset of the area. 
        auto protect(usize offset, usize size, MmuFlags flags) -> Status;

        /// Update rights of the subset of the area. 
        auto reserve(usize offset, usize size, MmuFlags flags, char const *name) -> Status;

        auto contains(VirtAddr addr) const -> bool;

        auto find_mapping_locked(VirtAddr va) -> VmMapping *;

        auto dump() const -> void;
      protected:
        static auto create_common(VirtAddr, usize, VmaFlags, VmArea *, VmAspace *, char const *, ustl::Rc<Self> *) 
            -> Status;

        VmArea(VirtAddr, usize, VmaFlags, VmArea *, VmAspace *, char const *);

        auto create_subaom_preamble(usize offset, usize size, VmaFlags vmaf, VmMapOption option) 
            -> ktl::Result<ustl::Pair<VirtAddr, usize>>;

        auto alloc_spot(usize size, usize align, VirtAddr lower_limit, VirtAddr upper_limit) 
            -> ktl::Result<VirtAddr>;

        auto map_with_vmo(VirtAddr *, usize, MmuFlags, ustl::Rc<VmObject>, VmMapOption, char const *)
            -> ktl::Result<ustl::Rc<VmMapping>>;
        
        auto dump_locked() const -> void;

        virtual auto destroy() -> Status override;
      private:
        friend class VmObject;
        friend class VmAspace;
        friend class VmMapping;
        friend class VmAreaOrMapping;

        usize m_num_mappings;
        VmaSet m_subvmas;
    };
} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_AREA_HPP