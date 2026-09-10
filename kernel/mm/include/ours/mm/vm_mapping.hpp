/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_MAPPING_HPP
#define OURS_MM_VM_MAPPING_HPP 1

#include <ours/mm/fault.hpp>
#include <ours/mm/priv/memory_allocation_policy.hpp>
#include <ours/mm/protection_ranges.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/status.hpp>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/fn.hpp>
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
    auto map(VirtAddr base, usize size, bool commit, MapControl) -> Status;

    auto protect(VirtAddr base, usize size, MmuFlags) -> Status;

    auto unmap(VirtAddr base, usize, UnmapControl) -> Status;

    /// Return the final mapping status and how many pages mapped.
    auto fault(VmFault *vmf) -> ustl::Pair<Status, usize>;

    VmMapping(VmArea *, VirtAddr, usize, VmaFlags, ustl::Arc<VmObject>, usize, char const *);
    virtual ~VmMapping() = default;

  private:
    friend class VmArea;
    friend class VmObject;

    auto map_paged(VmObjectPaged *vmo, VirtAddr base, usize size, bool commit, MapControl control)
        -> Status;

    auto map_physical(VmObjectPhysical *vmo, VirtAddr base, usize size, MapControl control)
        -> Status;

    /// Tear the pages of [base, base + size) down, assuming the aspace lock is
    /// already held.
    auto unmap_locked(VirtAddr base, usize size, UnmapControl control) -> Status;

    /// Rewrite the flags of [base, base + size), assuming the aspace lock is
    /// already held and the range was validated against this mapping.
    ///
    /// Split out for the same reason unmap_locked() is: get_lock() is the
    /// address space's single lock and it is not recursive, so a VmArea that
    /// already holds it while descending its children cannot call the public
    /// protect(), which takes it.
    auto protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status;

    virtual auto activate() -> Status override;
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
