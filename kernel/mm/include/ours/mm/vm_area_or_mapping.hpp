/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_AREA_OR_MAPPING_HPP
#define OURS_MM_VM_AREA_OR_MAPPING_HPP 1

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>
#include <ustl/collections/btree.hpp>
#include <ustl/collections/intrusive/any_hook.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/move.hpp>

#include <functional>

namespace ours::mm {
enum class VmMapOption {
    // For semenatics, no any effects.
    None,
    Commit = BIT(0),
    Pinned = BIT(1),
    Fixed = BIT(2),
    FixedNoReplace = BIT(3),
};
USTL_ENABLE_ENUM_BITMASK(VmMapOption);

enum class VmaFlags : u32 {
    // For semantics, no any actual effects.
    None,

    // Only serve for VmMapping
    Write = 0_bit,
    Read = 1_bit,
    Exec = 2_bit,
    Share = 3_bit,
    PermMask = Write | Read | Exec | Share,

    // States
    Active = 0x10000,
    Pinned = 0x20000,

    // Features.
    Anonymous = 0x40000,
    Mergeable = 0x80000,

    // Types
    Area = 0x0000'0000, // For semantics, no any actual effects.
    Mapping = 0x1000'0000,
};
USTL_ENABLE_ENUM_BITMASK(VmaFlags);

FORCE_INLINE constexpr auto extract_permissions(VmaFlags vmaflags) -> MmuFlags {
    MmuFlags mmuf{};
    if (!!(vmaflags & VmaFlags::Write)) {
        mmuf |= MmuFlags::Writable;
    }
    if (!!(vmaflags & VmaFlags::Read)) {
        mmuf |= MmuFlags::Readable;
    }
    if (!!(vmaflags & VmaFlags::Exec)) {
        mmuf |= MmuFlags::Executable;
    }

    return mmuf;
}

class VmAreaOrMapping: public ustl::RefCounted<VmAreaOrMapping, ustl::sync::ThreadSafePolicy> {
    using Self = VmAreaOrMapping;
    using Base = ustl::RefCounted<Self, ustl::sync::ThreadSafePolicy>;

    // Base::destroy() calls ~VmAreaOrMapping() through the base class on the
    // final release(); without this friend, that call is ill-formed since
    // the destructor below is protected.
    friend Base;

  public:
    class Builder;

    static constexpr std::uintptr_t TYPE_TAG = 1;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }

    using Flags = VmaFlags;

    FORCE_INLINE auto aspace() -> ustl::Arc<VmAspace> & {
        return m_aspace;
    }

    FORCE_INLINE auto base() const -> VirtAddr {
        return m_base;
    }

    FORCE_INLINE auto size() const -> VirtAddr {
        return m_size;
    }

    FORCE_INLINE auto is_active() const -> bool {
        return !!(m_vmaf & VmaFlags::Active);
    }

    FORCE_INLINE auto is_mapping() const -> bool {
        return !!(m_vmaf & VmaFlags::Mapping);
    }

    FORCE_INLINE auto get_lock() -> sync::SpinLock & {
        return m_aspace->get_lock();
    }

    FORCE_INLINE auto check_range(VirtAddr base, usize size) const -> bool {
        return base >= m_base && size <= m_size && (base - m_base) <= m_size - size;
    }

    /// This for the collections to compare two VMA.
    FORCE_INLINE friend auto operator<(Self const &x, Self const &y) noexcept -> bool {
        return x.m_base < y.m_base;
    }

    struct BaseOrder final {
        FORCE_INLINE auto operator()(Self const &x, Self const &y) const noexcept -> bool {
            return x.m_base < y.m_base;
        }
    };

    FORCE_INLINE auto end() const -> VirtAddr {
        return m_base + m_size;
    }

    /// Augmentation that lets `VmaSet::find_spot` locate a free gap by
    /// descending the tree instead of walking every area in order.
    ///
    /// The aggregate lives in the B-tree's nodes, not in the areas, so an
    /// area carries no per-node bookkeeping at all. A node folds its
    /// children and values strictly in key order, which is what makes the
    /// seam term below well defined: `combine(left, right)` really does mean
    /// "left is entirely below right in address order".
    struct GapAugment final {
        using Element = Self *;
        static constexpr auto EXTENSIONS = ustl::collections::intrusive::Extension::Augment;

        struct Aggregate {
            /// No areas underneath: the span terms are meaningless and the
            /// seam with a neighbour must not be computed.
            bool m_empty{true};
            VirtAddr m_min_start{};
            VirtAddr m_max_end{};
            VirtAddr m_max_gap{};

            friend auto operator==(Aggregate const &, Aggregate const &) -> bool = default;
        };

        static auto identity() noexcept -> Aggregate {
            return Aggregate{};
        }

        static auto of_element(Element area) noexcept -> Aggregate {
            return Aggregate{
                .m_empty = false,
                .m_min_start = area->base(),
                .m_max_end = area->end(),
                .m_max_gap = 0,
            };
        }

        static auto combine(Aggregate const &left, Aggregate const &right) noexcept -> Aggregate {
            if (left.m_empty) {
                return right;
            }
            if (right.m_empty) {
                return left;
            }

            auto max_gap = left.m_max_gap;
            if (right.m_max_gap > max_gap) {
                max_gap = right.m_max_gap;
            }
            // The seam the fold just closed: everything on the left ends by
            // `left.m_max_end`, everything on the right starts at
            // `right.m_min_start`.
            if (right.m_min_start > left.m_max_end) {
                auto const seam = right.m_min_start - left.m_max_end;
                if (seam > max_gap) {
                    max_gap = seam;
                }
            }
            return Aggregate{
                .m_empty = false,
                .m_min_start = left.m_min_start,
                .m_max_end = left.m_max_end > right.m_max_end ? left.m_max_end : right.m_max_end,
                .m_max_gap = max_gap,
            };
        }
    };

  protected:
    /// `magic` is the derived class's own `gktl::magic<Derived>()`, passed in
    /// because only the derived constructor knows what it is being built as,
    /// and the canary has to hold that from the start. Same shape, and for the
    /// same reason, as `VmObject`'s constructor.
    ///
    /// It used to be a pair of `gktl::magic("VmArea")`-style constants declared
    /// here, which gave each type two identities that disagreed in silence: the
    /// object carried the hash of the bare literal `"VmMapping"`, while every
    /// `Canary::verify<VmMapping>()` demanded the hash of the *qualified*
    /// `"ours::mm::VmMapping"` that `gktl::TypeName` recovers from
    /// `__PRETTY_FUNCTION__`. Both are `u32`, so no call site could show the
    /// difference; the mismatch surfaced only once the checks began to be
    /// spelt `verify<Self>()`, and then only at whichever of them the boot
    /// reached first. `VmObject` was cut over for exactly this and left a
    /// comment saying this class already had the right shape -- it did not.
    ///
    /// Seeding from the type rather than from a spelling of its name is what
    /// removes the failure mode, not merely this instance of it: there is one
    /// identity per type, and the constant that could drift out of step no
    /// longer exists to be written.
    VmAreaOrMapping(u32 magic, VirtAddr base, usize size, VmaFlags vmaf, VmArea *, VmAspace *,
                    char const *);
    ~VmAreaOrMapping() = default;

    /// Check whether a given `|mmuflags|` is valid. True if valid, otherwise invalid.
    auto validate_mmuflags(MmuFlags mmuf) const -> bool;

    /// Check whether a given `|vmaflags|` is valid. True if valid, otherwise invalid.
    FORCE_INLINE auto validate_vmaflags(VmaFlags vmaf) const -> bool {
        auto perm_allowed = (m_vmaf & VmaFlags::PermMask);
        if (((vmaf & VmaFlags::PermMask) | perm_allowed) != perm_allowed) {
            return false;
        }

        return true;
    }

    FORCE_INLINE auto mark_active() -> void {
        m_vmaf |= VmaFlags::Active;
    }

    virtual auto activate() -> Status;

    /// This routine by default just remove itself.
    virtual auto destroy() -> Status;

    friend class VmObject;
    friend class VmAspace;
    friend class VmaSet;
    friend class VmArea;

    gktl::Canary m_canary;
    char const *m_name;
    VirtAddr m_base;
    VirtAddr m_size;
    VmaFlags m_vmaf;
    VmArea *m_parent;
    ustl::Arc<VmAspace> m_aspace;
};

/// Areas are held by pointer: the tree allocates only its own node storage,
/// never the areas, which keep coming from the object cache as before. The
/// subtree aggregates live in the tree's nodes rather than in the areas.
struct VmaBaseOfPointer final {
    FORCE_INLINE auto operator()(VmAreaOrMapping *area) const noexcept -> VirtAddr {
        return area->base();
    }
};

class VmAreaOrMapping::Builder {
    using Self = VmAreaOrMapping::Builder;

  public:
    Builder(ustl::Arc<VmAspace> *aspace, VmaFlags vmaf, usize size);
    ~Builder() = default;

    auto build(ustl::Arc<VmAreaOrMapping> *out) -> Status;

    auto set_base(VirtAddr base) -> Self & {
        m_base = base;
        return *this;
    }

    auto set_parent(VmArea *parent) -> Self & {
        m_parent = parent;
        return *this;
    }

    auto set_aspace(ustl::Arc<VmAspace> *aspace) -> Self & {
        m_aspace = aspace;
        return *this;
    }

    auto set_vmo(ustl::Arc<VmObject> vmo) -> Self &;

    auto set_vmo_offset(usize vmo_ofs) -> Self & {
        m_vmo_ofs = vmo_ofs;
        return *this;
    }

    auto set_name(char const *nm) -> Self & {
        m_name = nm;
        return *this;
    }

    auto set_option(VmMapOption option) -> Self & {
        m_option = option;
        return *this;
    }

    auto set_mmuf(MmuFlags mmuf) -> Self & {
        m_mmuf = mmuf;
        return *this;
    }

  private:
    VirtAddr m_base;
    usize m_size;
    VmaFlags m_vmaf;
    MmuFlags m_mmuf;
    VmMapOption m_option;
    VmArea *m_parent;
    ustl::Arc<VmAspace> *m_aspace;
    char const *m_name;
    usize m_vmo_ofs;
    ustl::Arc<VmObject> m_vmo;
};

/// The set of areas a `VmArea` owns, keyed by base address.
///
/// The B-tree is a *member*, not a base, and that is load-bearing rather
/// than stylistic: the set holds a strong reference on every area it points
/// at, and inheriting would re-expose the tree's own `insert`/`erase`, which
/// take and drop pointers with no reference counting at all. A caller
/// reaching one of those would leave the tree pointing at an area that can
/// then be destroyed. Composition makes the retain/release pairing the only
/// way in or out.
class VmaSet {
    using Self = VmaSet;
    using Tree =
        ustl::collections::BTree<VirtAddr, VmAreaOrMapping *, VmaBaseOfPointer, std::less<>,
                                 ustl::collections::Allocator<VmAreaOrMapping *>,
                                 VmAreaOrMapping::GapAugment>;

  public:
    class Enumerator;
    using Element = VmAreaOrMapping *;
    using IterMut = Tree::iterator;
    using Iter = Tree::const_iterator;

    VmaSet() = default;
    VmaSet(VmaSet const &) = delete;
    auto operator=(VmaSet const &) -> VmaSet & = delete;

    /// Drops the reference the set holds on anything still present, so no
    /// area outlives the set only to be leaked, and none is left pointed at
    /// by a tree that is going away.
    ~VmaSet();

    [[nodiscard]]
    FORCE_INLINE auto empty() const -> bool {
        return m_tree.empty();
    }

    [[nodiscard]]
    FORCE_INLINE auto size() const -> usize {
        return m_tree.size();
    }

    FORCE_INLINE auto begin() -> IterMut {
        return m_tree.begin();
    }
    FORCE_INLINE auto begin() const -> Iter {
        return m_tree.begin();
    }
    FORCE_INLINE auto end() -> IterMut {
        return m_tree.end();
    }
    FORCE_INLINE auto end() const -> Iter {
        return m_tree.end();
    }

    /// Takes a reference once the area is actually in the tree. Insertion
    /// allocates node storage, so unlike the intrusive set this replaced it
    /// can fail; no reference is taken when it does, nor when an area with
    /// the same base is already present.
    auto insert(VmAreaOrMapping &value) -> Status;

    /// Removes `value` and drops the set's reference. The area may be
    /// destroyed by the time this returns, if nothing else holds it.
    auto erase(VmAreaOrMapping &value) -> void;

    /// Removes what `pos` names and returns the position after it, so a walk
    /// that erases as it goes keeps its place.
    ///
    /// The successor is not free: the underlying tree implements this by
    /// remembering the next key, erasing, and looking that key up again, since
    /// an erase can rebalance and no iterator survives that. So an erasing step
    /// costs O(log n) where a plain ++ costs O(1) amortised -- which is the
    /// reason to reach for this only on the steps that actually erase.
    ///
    /// Drops the set's reference, so the area may be gone when this returns
    /// unless the caller holds one of its own.
    auto erase(IterMut pos) -> IterMut;

    /// The first area that can cover an address at or above |addr|, or end().
    ///
    /// Two candidates and only one of them is the lower bound: the first area
    /// whose base is at or after |addr|, and the one before it, which starts
    /// lower and may still reach past |addr|. Anything before *that* ends at or
    /// below |addr|, because the areas are disjoint and ordered.
    ///
    /// Every range walk begins here -- Enumerator, has_range() and the erasing
    /// walk in VmArea::unmap_locked() -- because the step-back is the half that
    /// is easy to omit, and omitting it drops an area silently.
    auto seek(VirtAddr addr) -> IterMut;
    auto seek(VirtAddr addr) const -> Iter;

    /// Releases every area and empties the set.
    auto clear() -> void;

    /// Determine that in this set if a region contains the range [base, base + size).
    ///
    /// Return true if contains, otherwise false.
    auto has_range(VirtAddr base, usize size) const -> bool;

    auto find_spot(usize size, usize align, VirtAddr lower_bound, VirtAddr upper_bound) const
        -> ustl::Result<VirtAddr, Status>;

    /// The first area whose base is at or after |addr|, or NONE when every
    /// area starts below it.
    ///
    /// Keyed on base alone, which is the whole subtlety: an area starting
    /// below |addr| may still reach past it, so this is never the complete
    /// answer to "what covers |addr|". Every range walk has to look at the
    /// area before this one too -- see Enumerator's constructor and
    /// has_range(), which are the two places that reasoning is written out.
    FORCE_INLINE auto lower_bound(VirtAddr addr) -> ustl::Option<IterMut> {
        return m_tree.lower_bound(addr);
    }
    FORCE_INLINE auto lower_bound(VirtAddr addr) const -> ustl::Option<Iter> {
        return m_tree.lower_bound(addr);
    }

    /// Find the first existent VMA whose base > addr.
    FORCE_INLINE auto upper_bound(VirtAddr addr) -> ustl::Option<IterMut> {
        return m_tree.upper_bound(addr);
    }
    FORCE_INLINE auto upper_bound(VirtAddr addr) const -> ustl::Option<Iter> {
        return m_tree.upper_bound(addr);
    }

  private:
    Tree m_tree;
};

/// Walks the areas overlapping [min, max), in address order.
///
/// The seek is the entire point. The set is a B-tree keyed by base, so the
/// first area of a range is O(log n) away; every range operation on a VmArea
/// used to scan from the front instead, which made unmap, protect and -- worst
/// of all, since it runs on every page fault -- find_mapping_locked() linear in
/// the number of children.
///
/// Collecting the walk here rather than repeating it at each call site is not
/// only about not repeating code: the "area before the lower bound may reach
/// into the window" case is easy to leave out, and leaving it out loses a
/// child silently rather than failing.
class VmaSet::Enumerator {
  public:
    Enumerator(VmaSet &, VirtAddr min, VirtAddr max);
    Enumerator(VmArea &, VirtAddr min, VirtAddr max);
    ~Enumerator() = default;

    /// The next area overlapping the window, or nullptr once the walk is past
    /// it. Every area handed out genuinely overlaps; the caller does not have
    /// to re-check.
    ///
    /// The iterator advances before the area is returned, so a caller may drop
    /// its own reference to what it receives. It may not erase from the set:
    /// a B-tree erase can rebalance nodes and this iterator does not survive
    /// that, wherever in the tree the erase happened. VmArea::unmap_locked()
    /// shows how a walk that destroys its children is written instead.
    auto next() -> VmAreaOrMapping *;

  private:
    VmaSet *m_set;
    IterMut m_iter;
    VirtAddr m_max;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_AREA_OR_MAPPING_HPP
