/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_AREA_HPP
#define OURS_MM_VM_AREA_HPP 1

#include <ours/mm/object_cache.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ktl/result.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/pair.hpp>

namespace ours::mm {
/// Whether an unmap may destroy the sub-areas that lie wholly inside its range.
///
/// The kernel's own callers own every area they unmap in and pass Yes. A
/// program reaches an area through a VMA handle, and the handle's OpChildren
/// right is what lets it reach the areas below the one it names: without it a
/// range that covers a sub-area is refused, as one that only partly covers it
/// always is. What a sub-area holds is not the parent handle's to give away,
/// which is why the right is asked of the handle and not of the area.
///
/// A handle's Rights::OpChildren becomes it in
/// VmaDispatcher::op_children_from_rights().
enum class VmAreaOpChildren : bool {
    No,
    Yes,
};

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

    FORCE_INLINE
    static auto create(VirtAddr va, usize size, VmaFlags vmaf, VmArea *parent, VmAspace *aspace,
                       char const *name, ustl::Arc<Self> *out) -> Status {
        DEBUG_ASSERT(parent);
        return create_common(va, size, vmaf, parent, aspace, name, out);
    }

    FORCE_INLINE
    static auto create_root(VirtAddr va, usize size, VmaFlags vmaf, VmAspace *aspace,
                            char const *name, ustl::Arc<Self> *out) -> Status {
        return create_common(va, size, vmaf, nullptr, aspace, name, out);
    }

    /// Every address this class takes is an absolute virtual address, never an
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
    /// because both spellings were a bare u64.
    ///
    /// The one offset that stays an offset is |vmo_ofs|: it indexes into the
    /// object, which has no address of its own. Keeping exactly one offset in
    /// the interface is what makes it hard to mistake for the other.
    ///
    /// Internally the *_locked helpers were already absolute, so the
    /// conversions did not buy anything even inside the class.

    /// Create a sub-VMA at |where|, with the same placement rules as map_with_vmo().
    ///
    /// |vmaf| is the new area's allowance, and it may narrow this area's but
    /// never widen it. Narrowing is the point: it is the ceiling every
    /// protect() under the new area is checked against.
    auto create_subvma(VirtAddr where, usize size, VmaFlags vmaf, char const *name,
                       ustl::Arc<Self> *out) -> Status;

    /// The two coarse-grained ways to get memory mapped into this area. They
    /// differ in what backs the mapping, which is what their names say: a
    /// range of physical memory that already exists, or anonymous pages this
    /// creates. Where it lands is |where|'s business in both, and the address
    /// it landed on is the returned mapping's base().
    ///
    /// This used to be map_at() and map(), a pair whose names read as a
    /// placement distinction -- "at an address" versus not -- while the actual
    /// distinction was the backing; placement was a `VirtAddr *` argument both
    /// of them took. Two axes were spelt as one, and neither was spelt as
    /// itself.
    ///
    /// With |MapOptions::Resident| the mapping is populated immediately rather
    /// than left for a page fault to resolve.

    /// Map |size| bytes of physical memory starting at |phys_base|.
    auto map_phys(PhysAddr phys_base, VirtAddr where, usize size, MmuFlags, MapOptions,
                  char const *name) -> ktl::Result<ustl::Arc<VmMapping>>;

    /// Map |size| bytes of anonymous memory.
    auto map_anon(VirtAddr where, usize size, MmuFlags, MapOptions, char const *name)
        -> ktl::Result<ustl::Arc<VmMapping>>;

    /// Map |size| bytes of an existing VMO, starting at its byte offset |vmo_ofs|.
    /// VirtAddr::Anywhere chooses a free range. Zero is a concrete address.
    /// Other reserved values are InvArgs and an occupied range is AlreadyExists.
    ///
    /// |mmuf| is the initial protection. The mapping retains the permission
    /// ceiling |ceiling & permissions()| for later protect() calls.
    ///
    /// The mapping is registered before the selected mode installs entries.
    /// Lazy registers it for demand faults. A failed immediate installation
    /// tears the mapping down before returning its error.
    auto map_with_vmo(VirtAddr where, usize size, u64 vmo_ofs, MmuFlags mmuf, VmaFlags ceiling,
                      ustl::Arc<VmObject> vmo, MapOptions option, char const *name)
        -> ktl::Result<ustl::Arc<VmMapping>>;

    /// Tear down everything mapped in [base, base + size). AccessDenied, with
    /// nothing torn down, if a sealed mapping (MapOptions::Sealed) overlaps
    /// the range. A sub-area wholly inside the range is destroyed when
    /// |op_children| allows it and refuses the whole call (InvArgs) when it
    /// does not; one only partly inside always refuses it. BadState once this
    /// area has been destroyed, asked under the address space lock.
    ///
    /// destroy() does not look: it is the address space's own teardown (and
    /// the way a region is dissolved), and a vma_destroy that a program can
    /// call has to check the region for sealed mappings first, as this does.
    ///
    /// |op_children| defaults to Yes for the kernel's own callers, which own
    /// every area they unmap in; vma_unmap passes what the handle's rights say.
    auto unmap(VirtAddr base, usize size, VmAreaOpChildren op_children = VmAreaOpChildren::Yes)
        -> Status;

    /// Destroys |child|, and everything under it, if it is still one of this
    /// area's children: unmap() names a range, this names the object. The
    /// address space lock is taken here. BadState if it is not linked here
    /// any more: something destroyed it first -- an unmap() over its range,
    /// or the teardown of an area above it -- and whatever lies in its range
    /// now is not |child|.
    ///
    /// The caller holds a reference to |child| across the call. The question
    /// compares the object at |child|'s base with |child| by address, and
    /// without that reference a |child| destroyed in the meantime could have
    /// its memory reused for another object at the same base, which would
    /// then answer as |child|.
    ///
    /// For the call that made |child| and then could not hand it out, as the
    /// undo of that call (object::VmaDispatcher::undo_map() and
    /// undo_allocate()). The range is public from the moment the child is
    /// linked, so another holder of this area can unmap it and put something
    /// of its own in the same place before the undo runs; an undo by range
    /// would then destroy that instead.
    ///
    /// Seals are not asked, as destroy() does not ask them: a child its maker
    /// could not report has been promised to nobody, so its seal has promised
    /// nothing yet. A sealed mapping that was reported is not this call's to
    /// take; a program reaches only unmap(), which refuses it.
    auto destroy_child(VmAreaOrMapping &child) -> Status;

    /// Update rights of the subset of the area covering [base, base + size).
    auto protect(VirtAddr base, usize size, MmuFlags flags) -> Status;

    /// Claim [base, base + size) for an already-mapped range, so the VMM
    /// accounts for memory the boot path installed behind its back.
    auto reserve(VirtAddr base, usize size, MmuFlags flags, char const *name) -> Status;

    /// Walks every mapping overlapping [first, last] under this area, in
    /// address order, flattening nested sub-areas away.
    ///
    /// This is the shape that lets a range operation stop recursing. The
    /// operations themselves only ever act on leaves -- a sub-area owns no page
    /// tables, so protect() and unmap() reach it only to reach its children --
    /// which means the tree structure is bookkeeping the caller does not want
    /// to see. Flattening it here leaves protect_locked() and
    /// can_protect_locked() as plain loops.
    ///
    /// What replaces the call stack is |m_parent|: the walk descends into a
    /// sub-area and, when that sub-area is exhausted, climbs one link and
    /// re-finds where it was by the child's base, which is the tree's key.
    /// Nothing is remembered per level, so nesting costs no memory at all --
    /// and nothing bounds that nesting, since create_subvma() will build a
    /// sub-area inside a sub-area as deep as it is asked to.
    ///
    /// Like VmaSet::Enumerator, this must not be used by a walk that erases:
    /// a B-tree erase can rebalance and unseat the iterator, and the climb
    /// relies on the child it came out of still being findable by its base.
    ///
    /// Mappings only, deliberately. An enumeration that also yields sub-areas
    /// is what an address-space dump or a teardown accounting would want; it
    /// gets a mode of its own (areas and mappings, versus mappings only) when
    /// it gets a caller.
    class MappingEnumerator;

    auto contains(VirtAddr addr) const -> bool;

    TA_REQUIRES(get_lock())
    auto find_mapping_locked(VirtAddr va) -> VmMapping *;

    auto dump() const -> void;

  protected:
    static auto create_common(VirtAddr, usize, VmaFlags, VmArea *, VmAspace *, char const *,
                              ustl::Arc<Self> *) -> Status;

    VmArea(VirtAddr, usize, VmaFlags, VmArea *, VmAspace *, char const *);

    /// Settle where a new child goes and how big it is, or say why it cannot.
    ///
    /// Placement only. Permissions used to be checked here too, through a
    /// VmaFlags argument that both callers filled in with this area's own
    /// m_vmaf -- so the check compared m_vmaf against m_vmaf and passed
    /// unconditionally. The two callers do not even want the same check
    /// (an area is bounded by a VmaFlags allowance, a mapping by the MmuFlags
    /// it is being created with), which is why each now makes its own.
    ///
    /// |block_phase|, when there is one, is where in a block the child must
    /// start if the VMM places it: the object offset modulo a block, for a
    /// mapping of an object that takes blocks, so that the mapping's addresses
    /// and the object's offsets agree modulo a block and every whole block of
    /// the object can be mapped as one leaf (decision D6 of
    /// .agents/tasks/large-pages). A child placed by its creator ignores it.
    TA_REQUIRES(get_lock())
    auto create_subaom_preamble(VirtAddr where, usize size,
                                ustl::Option<usize> block_phase = ustl::NONE)
        -> ktl::Result<ustl::Pair<VirtAddr, usize>>;

    TA_REQUIRES(get_lock())
    auto alloc_spot(usize size, usize align, VirtAddr lower_limit, VirtAddr upper_limit)
        -> ktl::Result<VirtAddr>;

    auto dump_locked() const -> void;

    /// Unmap everything inside [base, base + size), assuming the aspace lock is
    /// already held and the range was validated against this area. See
    /// unmap() for |op_children|.
    TA_REQUIRES(get_lock())
    auto unmap_locked(VirtAddr base, usize size, VmAreaOpChildren op_children) -> Status;

    /// Whether a sealed mapping overlaps [base, base + size); the aspace lock
    /// held. See unmap().
    TA_REQUIRES(get_lock())
    auto has_sealed_mapping_locked(VirtAddr base, usize size) -> bool;

    /// Fan a protect() down to the children overlapping [base, base + size),
    /// assuming the aspace lock is already held.
    TA_REQUIRES(get_lock())
    auto protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status;

    /// Whether every child overlapping [base, base + size) would accept
    /// |mmuf|. Run before protect_locked() applies anything; see there.
    TA_REQUIRES(get_lock())
    auto can_protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> bool;

    TA_REQUIRES(get_lock())
    virtual auto destroy() -> Status override;

  private:
    /// Registers a mapping and leaves page-table installation to the caller.
    auto create_mapping(VirtAddr where, usize size, usize vmo_ofs, MmuFlags mmuf, VmaFlags ceiling,
                        ustl::Arc<VmObject> vmo, char const *name, MapOptions option,
                        ustl::Arc<VmMapping> *out) -> Status;

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

class VmArea::MappingEnumerator {
  public:
    /// [first, last] is inclusive at both ends, for the reason spelt out in
    /// VmAreaOrMapping::last(): an exclusive end cannot name a window that runs
    /// to the top of the address space, which is exactly what the whole of the
    /// kernel root VMA is.
    MappingEnumerator(VmArea &root, VirtAddr first, VirtAddr last);
    ~MappingEnumerator() = default;

    /// The next mapping overlapping the window, or nullptr once the walk is
    /// past it. Every mapping handed out genuinely overlaps; the caller clips
    /// the range itself, since only it knows what to do with the overlap.
    auto next() -> VmMapping *;

  private:
    /// Where the walk stops climbing. Not necessarily the root of the address
    /// space -- an operation on a sub-area enumerates only that sub-area.
    VmArea *m_root;
    /// The area whose children |m_iter| is positioned in.
    VmArea *m_area;
    VmaSet::IterMut m_iter;
    VirtAddr m_first;
    VirtAddr m_last;
};

class VmAreaOrMapping::NodeEnumerator {
  public:
    /// One node and how deep it lies.
    struct Node {
        VmAreaOrMapping *node;
        usize depth;
    };

    /// The children of |root| are handed out at |first_depth|, theirs one
    /// deeper; |root| itself is not handed out.
    NodeEnumerator(VmArea &root, usize first_depth);
    ~NodeEnumerator() = default;

    /// The next node, or one whose |node| is null once the walk is done.
    auto next() -> Node;

  private:
    VmArea *m_root;
    /// The area whose children |m_iter| is positioned in, and their depth.
    VmArea *m_area;
    VmaSet::IterMut m_iter;
    usize m_depth;
};

template <typename Visitor>
auto VmAreaOrMapping::descend(Visitor &&visit) -> void {
    visit_node(visit, *this, 0);
    if (is_mapping()) {
        return;
    }
    NodeEnumerator enumerator{static_cast<VmArea &>(*this), 1};
    while (true) {
        auto const [node, depth] = enumerator.next();
        if (node == nullptr) {
            return;
        }
        visit_node(visit, *node, depth);
    }
}
} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_AREA_HPP
