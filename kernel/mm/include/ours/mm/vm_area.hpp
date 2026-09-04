/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_AREA_HPP
#define OURS_MM_VM_AREA_HPP 1

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ktl/result.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/pair.hpp>

namespace ours::mm {
/// `VmArea` is a representation of a contiguous range of virtual memory space.
class VmArea final: public VmAreaOrMapping {
    using Self = VmArea;
    using Base = VmAreaOrMapping;

  public:
    static constexpr std::uintptr_t TYPE_TAG = 2;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }

    FORCE_INLINE static auto create(VirtAddr va, usize size, VmaFlags vmaf, VmArea *parent,
                                    VmAspace *aspace, char const *name, ustl::Arc<Self> *out)
        -> Status {
        DEBUG_ASSERT(parent);
        return create_common(va, size, vmaf, parent, aspace, name, out);
    }

    FORCE_INLINE static auto create_root(VirtAddr va, usize size, VmaFlags vmaf, VmAspace *aspace,
                                         char const *name, ustl::Arc<Self> *out) -> Status {
        return create_common(va, size, vmaf, nullptr, aspace, name, out);
    }

    /// Every range this class takes is an absolute virtual address, never an
    /// offset from |m_base|.
    ///
    /// It used to be split: unmap/protect/reserve/create_subvma took offsets
    /// while map/map_at handed absolute addresses back and
    /// contains/find_mapping_locked took them, so callers converted in both
    /// directions -- init_vmm.cpp literally wrote
    /// `phys_vma.base - root_vma->base()`. Two of those conversions were
    /// wrong: VmArea::unmap_locked() handed VmMapping::unmap_locked() an
    /// offset where it wanted an address, and create_mapping() passed its VMA
    /// offset as the VMO offset. Neither could be seen at the call site,
    /// because both spellings are a bare u64.
    ///
    /// The one offset that stays an offset is |vmo_ofs|: it indexes into the
    /// object, which has no address of its own. Keeping exactly one offset in
    /// the interface is what makes it hard to mistake for the other.
    ///
    /// Internally the *_locked helpers were already absolute, so the
    /// conversions did not buy anything even inside the class.

    /// Create a mapping over [base, base + size).
    auto create_mapping(VirtAddr base, usize size, usize vmo_ofs, MmuFlags mmuf,
                        ustl::Arc<VmObject> vmo, char const *name, VmMapOption option,
                        ustl::Arc<VmMapping> *out) -> Status;

    /// Create a sub-VMA over [base, base + size).
    auto create_subvma(VirtAddr base, usize size, VmaFlags vmaf, char const *name,
                       VmMapOption option, ustl::Arc<Self> *out) -> Status;

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
    auto map_at(PhysAddr phys_base, VirtAddr *virt_base, usize size, MmuFlags, VmMapOption,
                char const *name) -> ktl::Result<ustl::Arc<VmMapping>>;

    /// Map |size| bytes space to virtual address |virt_base|.
    auto map(VirtAddr *base, usize size, MmuFlags, VmMapOption, char const *name)
        -> ktl::Result<ustl::Arc<VmMapping>>;

    /// Tear down everything mapped in [base, base + size).
    auto unmap(VirtAddr base, usize size) -> Status;

    /// Update rights of the subset of the area covering [base, base + size).
    auto protect(VirtAddr base, usize size, MmuFlags flags) -> Status;

    /// Claim [base, base + size) for an already-mapped range, so the VMM
    /// accounts for memory the boot path installed behind its back.
    auto reserve(VirtAddr base, usize size, MmuFlags flags, char const *name) -> Status;

    auto contains(VirtAddr addr) const -> bool;

    auto find_mapping_locked(VirtAddr va) -> VmMapping *;

    auto dump() const -> void;

  protected:
    static auto create_common(VirtAddr, usize, VmaFlags, VmArea *, VmAspace *, char const *,
                              ustl::Arc<Self> *) -> Status;

    VmArea(VirtAddr, usize, VmaFlags, VmArea *, VmAspace *, char const *);

    auto create_subaom_preamble(VirtAddr base, usize size, VmaFlags vmaf, VmMapOption option)
        -> ktl::Result<ustl::Pair<VirtAddr, usize>>;

    auto alloc_spot(usize size, usize align, VirtAddr lower_limit, VirtAddr upper_limit)
        -> ktl::Result<VirtAddr>;

    auto map_with_vmo(VirtAddr *, usize, MmuFlags, ustl::Arc<VmObject>, VmMapOption, char const *)
        -> ktl::Result<ustl::Arc<VmMapping>>;

    auto dump_locked() const -> void;

    /// Unmap everything inside [base, base + size), assuming the aspace lock is
    /// already held and the range was validated against this area.
    auto unmap_locked(VirtAddr base, usize size) -> Status;

    /// Fan a protect() down to the children overlapping [base, base + size),
    /// assuming the aspace lock is already held.
    auto protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status;

    /// Whether every child overlapping [base, base + size) would accept
    /// |mmuf|. Run before protect_locked() applies anything; see there.
    auto can_protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> bool;

    virtual auto destroy() -> Status override;

  private:
    // ObjectCache::Instance placement-news T; VmArea's constructor stays
    // protected otherwise.
    template <typename, typename...>
    friend class ObjectCache::Instance;

    friend class VmObject;
    friend class VmAspace;
    friend class VmMapping;
    friend class VmAreaOrMapping;
    // For Enumerator's VmArea overload, which reaches m_subvmas.
    friend class VmaSet;

    usize m_num_mappings;
    VmaSet m_subvmas;
};
} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_AREA_HPP
