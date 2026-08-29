/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_MAPPING_HPP
#define OURS_MM_VM_MAPPING_HPP 1

#include <gktl/canary.hpp>
#include <ours/mm/fault.hpp>
#include <ours/mm/priv/memory_allocation_policy.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/status.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/limits.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/pair.hpp>

#include <algorithm>

namespace ours::mm {
/// Helper class to trace the mapping status in region. Its internal implementation
/// is based on multiple adjacent intervals whose left bound is the end of the previous one.
class MappingProtectionRanges {
    using Self = MappingProtectionRanges;

  public:
    class Enumerator;

    auto init(VirtAddr end, MmuFlags) -> Status;

    /// Update MMU flags for the range [base, base + size)
    ///
    /// Return a enumerator that can assist us to for each all regions updated.
    auto update(VirtAddr base, usize size, MmuFlags, VirtAddr lower_bound, VirtAddr upper_bound)
        -> Status;

    /// Make an enumerator which can enumerates all regions in range [base, base + size)
    auto make_enumerator(VirtAddr base, usize size) -> Enumerator;

    struct Region: public ustl::collections::intrusive::SetBaseHook<> {
        typedef Region Self;
        typedef ustl::collections::intrusive::SetBaseHook<> Base;

        Region() = default;
        Region(VirtAddr end, MmuFlags mmuf) : Base(), end(end), mmuf(mmuf) {};

        /// The right (exclusive) bound of this region.  The left bound is
        /// implicitly the previous region's `end` (or the mapping's base
        /// address for the first region).  This implicit-left-bound
        /// invariant is enforced only by correct usage of insert/erase —
        /// nothing checks it at compile time or runtime.
        VirtAddr end;
        MmuFlags mmuf;
    };

    // FIXME: Region is publicly exposed but is an internal implementation
    // detail of the implicit-left-bound interval tree.  If the underlying
    // data structure changes (e.g. to a radix tree or flat array), every
    // caller that depends on Region / Enumerator::NextResult breaks.
    // Consider moving Region into a priv/ detail header or using PIMPL.

    // FIXME: operator< considers only `end`, so two Region nodes with the
    // same end address are treated as equivalent by the tree.  The code
    // assumes uniqueness of `end` values, but MultiSet silently permits
    // duplicates — a bug that inserts a duplicate-end region would not be
    // caught and would corrupt the implicit-left-bound invariant.  Use Set
    // instead of MultiSet so the data structure rejects duplicates for you.
    struct RegionOrder final {
        FORCE_INLINE auto operator()(Region const &x, Region const &y) const noexcept -> bool {
            return x.end < y.end;
        }
    };

  private:
    template <typename... Args>
    static auto alloc_region(Args &&...) -> Region *;

    static auto free_region(Region *) -> void;

    typedef ustl::collections::intrusive::Multiset<
        Region, ustl::collections::intrusive::Compare<RegionOrder>>
        RegionList;
    RegionList regions_;
};

class MappingProtectionRanges::Enumerator {
    using Self = MappingProtectionRanges::Enumerator;

  public:
    Enumerator(VirtAddr base, RegionList::IterMut first, RegionList::IterMut last)
        : Self(base, first, last, 0, ustl::NumericLimits<VirtAddr>::max()) {}

    Enumerator(VirtAddr base, RegionList::IterMut first, RegionList::IterMut last,
               VirtAddr lower_bound, VirtAddr upper_bound)
        : base_(base)
        , lower_bound_(lower_bound)
        , upper_bound_(upper_bound)
        , iter_(first)
        , end_(last) {}

    ~Enumerator() = default;

    FORCE_INLINE auto shift(usize delta) -> void {
        // TODO: Enumerator::shift() is defined but never called anywhere
        // in the codebase.  Either wire it up in a consumer or remove it.
        base_ += delta;
    }

    struct NextResult {
        VirtAddr base;
        usize size;
        MmuFlags mmuf;
    };

    FORCE_INLINE auto next() -> ustl::Option<NextResult> {
        if (iter_ == end_ || base_ >= upper_bound_) {
            return ustl::none();
        }
        auto obase = std::max(base_, lower_bound_);
        auto mmuf = iter_->mmuf;

        base_ = std::min(iter_->end, upper_bound_);
        ++iter_;
        return ustl::some(NextResult{obase, base_ - obase, mmuf});
    }

    FORCE_INLINE friend auto operator!=(Self const &x, Self const &y) -> bool {
        return x.iter_ != y.iter_;
    }

  private:
    VirtAddr base_;
    VirtAddr lower_bound_;
    VirtAddr upper_bound_;
    RegionList::IterMut iter_;
    RegionList::IterMut end_;
};

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

    auto map(VirtAddr offset, usize size, bool commit, MapControl) -> Status;

    auto protect(VirtAddr offset, usize size, MmuFlags) -> Status;

    auto unmap(VirtAddr offset, usize, UnmapControl) -> Status;

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

    virtual auto activate() -> Status override;
    virtual auto destroy() -> Status override;

    ustl::Arc<VmObject> m_vmo;
    usize m_vmo_ofs;
    // TODO: m_policy (MemoryAllocationPolicy) is stored but never read by
    // any code path.  It is intended to guide NUMA / page-coloring
    // decisions during commit — either implement the integration or remove
    // the field to avoid dead weight.
    ustl::Arc<MemoryAllocationPolicy> m_policy;
    MappingProtectionRanges m_protection_ranges;
    ustl::collections::intrusive::ListMemberHook<> m_list_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_list_hook, VmoListOption);

  public:
    USTL_DECLARE_LIST(VmMapping, List, VmMapping::VmoListOption);
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_MAPPING_HPP
