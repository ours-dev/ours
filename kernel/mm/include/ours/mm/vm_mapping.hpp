/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_MAPPING_HPP
#define OURS_MM_VM_MAPPING_HPP 1

#include <ours/mm/priv/memory_allocation_policy.hpp>
#include <ours/mm/protection_ranges.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/limits.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/pair.hpp>

namespace ours::mm {
/// VmMapping is the representation of a or a group of area which has been mapped in
/// virtual memory address space.
///
/// It was usually created by VmArea out of some mapping request.
class VmMapping final: public VmAreaOrMapping {
    using Base = VmAreaOrMapping;
    using Self = VmMapping;

  public:
    static constexpr std::uintptr_t TYPE_TAG = 3;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }

    static auto create(VmArea *, VirtAddr, usize, VmaFlags, ustl::Arc<VmObject>, usize, MmuFlags,
                       char const *, ustl::Arc<Self> *) -> Status;

    /// [base, base + size) is an absolute address range, as it is throughout
    /// VmArea and as it already was in the unmap_locked()/protect_locked()
    /// halves below. These three took an offset from |m_base| instead, so
    /// every call across the VmArea/VmMapping seam converted -- and
    /// VmArea::unmap_locked() converted the wrong way, subtracting the base
    /// before calling the absolute unmap_locked() and silently naming a range
    /// at the bottom of the address space.
    auto map(VirtAddr base, usize size, bool commit, MapOptions) -> Status;

    auto protect(VirtAddr base, usize size, MmuFlags) -> Status;

    auto unmap(VirtAddr base, usize, UnmapOptions) -> Status;

    /// Return the final mapping status and how many pages mapped.
    TA_REQUIRES(get_lock())
    auto page_fault(VmFault *vmf) -> ustl::Pair<Status, usize>;

    /// The object this mapping shows, and where in it the mapping starts.
    /// Both are fixed when the mapping is made, so they are read without a
    /// lock. A reference of the caller's own, which keeps the object for a
    /// caller that goes on after the mapping is gone.
    [[nodiscard]]
    auto vmo() const -> ustl::Arc<VmObject>;

    [[nodiscard]]
    auto vmo_offset() const -> usize {
        return m_vmo_ofs;
    }

    /// Calls |visit|(base, size, mmuf) for each protection range of this
    /// mapping, in address order; together they tile it, and a range with no
    /// permission is one that was unmapped. Under the aspace lock, which
    /// every writer of the ranges holds.
    template <typename Visitor>
    TA_REQUIRES(get_lock())
    auto for_each_protection_range_locked(Visitor &&visit) -> void {
        auto enumerator = m_protection_ranges.make_enumerator(base(), size());
        while (auto const range = enumerator.next()) {
            visit(range.base, range.size, range.mmuf);
        }
    }

    VmMapping(VmArea *, VirtAddr, usize, VmaFlags, ustl::Arc<VmObject>, usize, char const *);
    virtual ~VmMapping() = default;

  private:
    friend class VmArea;
    friend class VmObject;

    /// Under the address space lock map() holds.
    TA_REQUIRES(get_lock())
    auto map_paged(VmObjectPaged *vmo, VirtAddr base, usize size, bool commit, MapOptions options)
        -> Status;

    auto map_physical(VmObjectPhysical *vmo, VirtAddr base, usize size, MapOptions options)
        -> Status;

    /// Tear the pages of [base, base + size) down, assuming the aspace lock is
    /// already held.
    TA_REQUIRES(get_lock())
    auto unmap_locked(VirtAddr base, usize size, UnmapOptions options) -> Status;

    /// Rewrite the flags of [base, base + size), assuming the aspace lock is
    /// already held and the range was validated against this mapping.
    ///
    /// Split out for the same reason unmap_locked() is: get_lock() is the
    /// address space's single lock and it is not recursive, so a VmArea that
    /// already holds it while descending its children cannot call the public
    /// protect(), which takes it.
    TA_REQUIRES(get_lock())
    auto protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status;

    /// Makes |change| to the page tables of the part of this mapping that
    /// shows [vmo_offset, vmo_offset + len) of its object; see
    /// VmObject::change_range_locked(). Called with the object's lock held and
    /// without the address space's.
    ///
    /// Only the page tables change. The record of what the mapping allows
    /// keeps what it had, because the mapping still allows it: what changed is
    /// that an access now faults, and the page is copied, taken or found anew
    /// first.
    ///
    /// Safe without the address space's lock for three reasons. The mapping is
    /// on the object's list, which it leaves under the object's lock before it
    /// is destroyed, so it cannot be gone. Its base, size and offset in the
    /// object never change after it is made. And the record it reads is
    /// written only under the object's lock as well (protect_locked(),
    /// unmap_locked()). The page table takes its own lock.
    ///
    /// A failure would leave a shared page writable, or an entry to a frame
    /// the object no longer has, and cannot be undone, so it stops the kernel.
    auto change_range_locked_object(u64 vmo_offset, u64 len, RangeChange change) -> void;

    /// Gives a present entry a permission an access needs and it lacks; see the
    /// definition. Part of page_fault(), split out for its length.
    TA_REQUIRES(get_lock())
    auto widen_present_locked(VmFault *vmf, VirtAddr va, PhysAddr existing, MmuFlags mmuf,
                              bool will_write) -> ustl::Pair<Status, usize>;

    /// Replaces the entry at |va| with one for |pa| and |mmuf|: unmap, then map.
    TA_REQUIRES(get_lock())
    auto remap_locked(VirtAddr va, PhysAddr pa, MmuFlags mmuf) -> ustl::Pair<Status, usize>;

    /// Maps the 2 MiB of addresses at |window| as one leaf over a block of
    /// |vmo|'s pages, when everything allows one; see the definition. None when
    /// something does not, and the caller maps page by page as before; the
    /// status of the map otherwise, with the pages it mapped. |vmo|'s lock is
    /// held, |mmuf| is what this mapping allows at |window|.
    TA_REQUIRES(get_lock())
    auto map_block_locked(VmObjectPaged &vmo, VirtAddr window, MmuFlags mmuf)
        -> ustl::Option<ustl::Pair<Status, usize>>;

    TA_REQUIRES(get_lock())
    virtual auto activate() -> Status override;
    TA_REQUIRES(get_lock())
    virtual auto destroy() -> Status override;

    ustl::Arc<VmObject> m_vmo;
    usize m_vmo_ofs;
    // TODO: m_policy (MemoryAllocationPolicy) is stored but never read by
    // any code path.  It is intended to guide NUMA / page-coloring
    // decisions during commit — either implement the integration or remove
    // the field to avoid dead weight.
    ustl::Arc<MemoryAllocationPolicy> m_policy;
    ProtectionRanges m_protection_ranges;
    ustl::collections::intrusive::ListMemberHook<> m_list_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_list_hook, VmoListOption);

  public:
    USTL_DECLARE_LIST(VmMapping, List, VmMapping::VmoListOption);
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_MAPPING_HPP
