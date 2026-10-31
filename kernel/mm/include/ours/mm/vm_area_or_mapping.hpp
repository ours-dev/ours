/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_AREA_OR_MAPPING_HPP
#define OURS_MM_VM_AREA_OR_MAPPING_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>

#include <functional>
#include <type_traits>

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ustl/collections/btree.hpp>
#include <ustl/collections/intrusive/any_hook.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/move.hpp>

namespace ours::mm {
/// What a caller asks of ONE operation that creates a mapping: options of the
/// call, not properties of the result.
///
/// It is kept apart from VmaFlags on purpose, and the line between them is
/// this: a VmMapOption is a request, a VmaFlags is what the object is. Some
/// requests leave nothing behind -- Commit means "populate the pages now" and
/// is gone once it has been done -- so they have no flag to become. Others
/// outlive the call: Pinned and Sealed are asked for here and, at creation
/// (VmArea::create_mapping()), become VmaFlags::Pinned and VmaFlags::Sealed on
/// the mapping. The same word on both sides is that translation, not a
/// duplicate: "make it sealed" and "it is sealed" are different statements.
///
/// One set of flags for both would also work; this project keeps the request
/// and the state apart. The ABI has a third vocabulary, ours::VmOptions, for
/// the same reason at another boundary.
enum class VmMapOption {
    // For semenatics, no any effects.
    None,
    /// Fill the page tables in when the mapping is made, rather than leaving
    /// each page to the first access that faults on it. Only that: it maps what
    /// the object has, and does not ask the object for pages of its own, which
    /// on a copy-on-write clone would copy every shared page. The ABI's
    /// ours::VmOptions::MapRange is what becomes it. It was called Commit,
    /// which is the name of the other thing.
    Populate = 0_bit,
    Pinned = 1_bit,

    /// The mapping is immutable from the moment it exists: nothing may unmap
    /// it, change its protection, or destroy it with its parent. See
    /// VmaFlags::Sealed.
    Sealed = 2_bit,
};
USTL_ENABLE_ENUM_BITMASK(VmMapOption);

/// Where a new area or mapping is to go.
///
/// Two states and no way to write a third, which is the whole point. The
/// address used to be a `VirtAddr *` that meant three things at once -- an
/// input hint, the output result, and, when null, "you choose" -- with a
/// separate VmMapOption::Fixed deciding whether the input half counted at all.
/// A caller who meant "anywhere" wrote a literal 0, and 0 is also a perfectly
/// good address, so the request that put the local APIC's MMIO on top of the
/// physmap was indistinguishable from a deliberate one (see section 36 of
/// docs/repairs-20260830.md). Naming the intent instead of encoding it in a
/// pointer's nullness makes that request unwritable.
///
/// The output half is gone rather than moved: the mapping this produces
/// already knows where it is, and `(*result)->base()` is the answer. Two of
/// the five call sites read it that way already and ignored the out-parameter
/// they had passed.
///
/// A flag beside an address is the shape this replaces: the address is
/// meaningless without the flag, and a null address can stand for "anywhere"
/// only where nothing ever maps page 0. So the address is optional instead:
/// at() holds one and anywhere() holds none, and "fixed" is whether there is
/// one, not a second fact stored beside it.
class VmPlacement {
    using Self = VmPlacement;

  public:
    /// Any free spot inside the parent will do; the VMM picks.
    FORCE_INLINE
    static constexpr auto anywhere() -> Self {
        return Self{};
    }

    /// This address or nothing. A range already spoken for is refused with
    /// Status::AlreadyExists rather than replaced: replacement is not
    /// implemented, and when it is, it gets its own named constructor rather
    /// than a second meaning for this one.
    FORCE_INLINE
    static constexpr auto at(VirtAddr base) -> Self {
        return Self{base};
    }

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto is_fixed() const -> bool {
        return m_base.is_some();
    }

    /// Only meaningful when is_fixed(); anywhere() has no address to give,
    /// and asking it for one traps.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto base() const -> VirtAddr {
        return m_base.value();
    }

  private:
    constexpr VmPlacement() = default;
    constexpr explicit VmPlacement(VirtAddr base) : m_base(base) {}

    ustl::Option<VirtAddr> m_base;
};

/// What a VMA -- an area or a mapping -- IS: its attributes and state, stored on
/// the object and true for as long as it lives. The permissions it allows
/// (which a mapping's protection may never exceed), whether it is active,
/// pinned or sealed, what kind of thing it is.
///
/// Not what a caller asked of a call: that is VmMapOption, and a request that
/// outlives the call is translated into a flag here at creation. See
/// VmMapOption.
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

    // Set once, when a mapping is created with VmMapOption::Sealed, and never
    // cleared: an unmap, protect or destroy that meets a sealed mapping is
    // refused (AccessDenied) whole, without touching anything else in its
    // range. The address space's own teardown is not such an operation; it
    // is the kernel taking the space away, not a program changing its shape.
    // Read under the address space lock and never written after the mapping
    // is linked, so it needs nothing more.
    Sealed = 0x100000,

    // Only serves VmArea: whether a mapping or sub-area may go at a place its
    // creator chose (VmPlacement::at). Without it everything in the area is
    // placed by the VMM, the one placement that randomisation can reach, so a
    // program handed such an area cannot pin anything at an address it picked.
    // VmArea::reserve() is a placement like any other and asks it too. A
    // mapping never carries it.
    // Every root area has it. A sub-area has it when it is made with it,
    // whether or not its parent does: the sub-area itself was placed by the
    // VMM, so places chosen inside it fix only where things lie relative to
    // each other. Set when the area is made and never changed.
    CanMapSpecific = 0x200000,

    // Types
    Area = 0x0000'0000, // For semantics, no any actual effects.
    Mapping = 0x1000'0000,
};
USTL_ENABLE_ENUM_BITMASK(VmaFlags);

FORCE_INLINE
constexpr auto extract_permissions(VmaFlags vmaflags) -> MmuFlags {
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

/// extract_permissions() the other way: the VmaFlags permission bits that
/// |mmuf| asks for. What a ceiling is compared against.
FORCE_INLINE
constexpr auto permissions_of(MmuFlags mmuf) -> VmaFlags {
    VmaFlags vmaf{};
    if (!!(mmuf & MmuFlags::Readable)) {
        vmaf |= VmaFlags::Read;
    }
    if (!!(mmuf & MmuFlags::Writable)) {
        vmaf |= VmaFlags::Write;
    }
    if (!!(mmuf & MmuFlags::Executable)) {
        vmaf |= VmaFlags::Exec;
    }
    return vmaf;
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

    FORCE_INLINE
    auto aspace() -> ustl::Arc<VmAspace> & {
        return m_aspace;
    }

    FORCE_INLINE
    auto base() const -> VirtAddr {
        return m_base;
    }

    FORCE_INLINE
    auto size() const -> VirtAddr {
        return m_size;
    }

    /// The name it was built with. Every name is a string literal today, so
    /// the pointer outlives the node; one copied from a caller would have to
    /// be stored, not borrowed, before this could hand it out.
    [[nodiscard]]
    FORCE_INLINE
    auto name() const -> char const * {
        return m_name;
    }

    /// What this area allows mappings under it, or what this mapping's
    /// protection may ever be raised to: the ceiling, as permission bits.
    /// Read without the aspace lock: the permission bits are fixed when the
    /// object is built, and only the state bits (Active) change after.
    [[nodiscard]]
    FORCE_INLINE
    auto permissions() const -> VmaFlags {
        return m_vmaf & VmaFlags::PermMask;
    }

    FORCE_INLINE
    auto is_sealed() const -> bool {
        return !!(m_vmaf & VmaFlags::Sealed);
    }

    [[nodiscard]]
    FORCE_INLINE
    auto is_active() const -> bool {
        return !!(m_vmaf & VmaFlags::Active);
    }

    FORCE_INLINE
    auto is_mapping() const -> bool {
        return !!(m_vmaf & VmaFlags::Mapping);
    }

    TA_RETURN_CAPABILITY(m_aspace->get_lock()) FORCE_INLINE auto get_lock() -> sync::Mutex & {
        return m_aspace->get_lock();
    }

    /// Walks every area and mapping under an area, depth first in address
    /// order, each with its depth: the shape of an address-space dump. An
    /// area is handed out before its children. The same climb by base as
    /// VmArea::MappingEnumerator, so it keeps no stack whatever the nesting,
    /// and like it must not be used by a walk that erases. Most callers want
    /// descend().
    class NodeEnumerator;

    /// Calls |visit| for this node and every node under it, depth first in
    /// address order: this one first, each area before its children. A mapping
    /// has nothing under it and is visited alone.
    ///
    /// What a visitor looks like, in order of preference:
    ///
    ///   - one that takes a VmAreaOrMapping &, and tells the two apart itself
    ///     (is_mapping());
    ///   - one that has an overload for each kind, a VmMapping & and a VmArea
    ///     &. Both are required, so that a kind added to a walk by mistake or
    ///     left out of it is a compile error and not a silent skip; a visitor
    ///     with nothing to do for one writes it empty.
    ///
    /// Each also takes, after the node, a `usize depth` if it wants one: 0 for
    /// this node, one more for each level under it.
    ///
    /// Under the address space's lock, which |visit| runs with held; it must
    /// not add or remove nodes. Defined in vm_area.hpp, which a caller
    /// includes.
    template <typename Visitor>
    auto descend(Visitor &&visit) -> void;

    FORCE_INLINE
    auto check_range(VirtAddr base, usize size) const -> bool {
        return base >= m_base && size <= m_size && (base - m_base) <= m_size - size;
    }

    /// This for the collections to compare two VMA.
    FORCE_INLINE
    friend auto operator<(Self const &x, Self const &y) noexcept -> bool {
        return x.m_base < y.m_base;
    }

    struct BaseOrder final {
        FORCE_INLINE
        auto operator()(Self const &x, Self const &y) const noexcept -> bool {
            return x.m_base < y.m_base;
        }
    };

    /// The last address inside the area, rather than the first one past it.
    ///
    /// There is no exclusive end to be had here. The kernel root VMA is
    /// [0xFFFFFF8000000000, 0x8000000000) and that sum is exactly 2^64, so
    /// `m_base + m_size` wraps to 0: every "starts below the end" test against
    /// the root answered no, and a walk over the whole root came back with no
    /// children at all. An inclusive last address cannot wrap while the area is
    /// non-empty, and every area is -- create_subaom_preamble() rejects a
    /// zero-length request before there is an area to ask.
    ///
    /// Containment is deliberately not written in these terms: contains() and
    /// check_range() stay in offset form, which is wrap-free for the same
    /// reason and reads better for "is this address mine". This is the other
    /// half of the job -- comparing one area's extent against another's, where
    /// an offset would first have to be rebased onto a common origin and would
    /// stop reading like an address.
    FORCE_INLINE
    auto last() const -> VirtAddr {
        DEBUG_ASSERT(m_size, "an empty area has no last address");
        return m_base + m_size - 1;
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
            ///
            /// The aggregate's one yes/no fact, so it has no word to share.
            /// An impossible span (start above last) could stand for it, but
            /// every reader would then have to know the trick.
            bool m_empty{true};
            VirtAddr m_min_start{};
            VirtAddr m_max_last{};
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
                .m_max_last = area->last(),
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
            // The seam the fold just closed: everything on the left ends at
            // `left.m_max_last`, everything on the right starts at
            // `right.m_min_start`. Spelt as a subtraction rather than as
            // `left.m_max_last + 1 < right.m_min_start`, because that increment
            // is precisely the wrap the inclusive form exists to avoid. Two
            // adjacent extents give a seam of zero, which no request can want,
            // so they need no case of their own.
            if (right.m_min_start > left.m_max_last) {
                auto const seam = right.m_min_start - left.m_max_last - 1;
                if (seam > max_gap) {
                    max_gap = seam;
                }
            }
            return Aggregate{
                .m_empty = false,
                .m_min_start = left.m_min_start,
                .m_max_last =
                    left.m_max_last > right.m_max_last ? left.m_max_last : right.m_max_last,
                .m_max_gap = max_gap,
            };
        }
    };

  private:
    /// |visit| called with |node| and, when it takes one, |depth|.
    template <typename Node, typename Visitor>
    static auto call_visitor(Visitor &visit, Node &node, usize const depth) -> void {
        if constexpr (std::is_invocable_v<Visitor &, Node &, usize>) {
            visit(node, depth);
        } else {
            static_assert(std::is_invocable_v<Visitor &, Node &>,
                          "a visitor of descend() takes the node, and optionally its depth");
            visit(node);
        }
    }

    /// |node| as the kind it is, which the caller has found out: a cast whose
    /// target is a template argument, so that it is checked where it is used
    /// and not here, where VmMapping is not yet complete.
    template <typename Kind>
    static auto downcast(VmAreaOrMapping &node) -> Kind & {
        return static_cast<Kind &>(node);
    }

    /// Hands |node| to |visit| by the shape descend() documents.
    template <typename Visitor>
    static auto visit_node(Visitor &visit, VmAreaOrMapping &node, usize const depth) -> void {
        if constexpr (std::is_invocable_v<Visitor &, VmAreaOrMapping &, usize> ||
                      std::is_invocable_v<Visitor &, VmAreaOrMapping &>) {
            call_visitor(visit, node, depth);
        } else if (node.is_mapping()) {
            call_visitor(visit, downcast<VmMapping>(node), depth);
        } else {
            call_visitor(visit, downcast<VmArea>(node), depth);
        }
    }

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
    FORCE_INLINE
    auto validate_vmaflags(VmaFlags vmaf) const -> bool {
        auto perm_allowed = (m_vmaf & VmaFlags::PermMask);
        if (((vmaf & VmaFlags::PermMask) | perm_allowed) != perm_allowed) {
            return false;
        }

        return true;
    }

    FORCE_INLINE
    auto mark_active() -> void {
        m_vmaf |= VmaFlags::Active;
    }

    TA_REQUIRES(get_lock())
    virtual auto activate() -> Status;

    /// This routine by default just remove itself.
    TA_REQUIRES(get_lock())
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
    FORCE_INLINE
    auto operator()(VmAreaOrMapping *area) const noexcept -> VirtAddr {
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
    // Default-initialised because not every path sets every field: a sub-area
    // has no VMO and therefore no VMO offset and no MMU flags, and build()
    // never reads those in that branch. Leaving them indeterminate meant the
    // only thing keeping the sub-area path honest was that it happened to set
    // the VMO offset to something -- the VMA offset, as it turned out.
    VirtAddr m_base{};
    usize m_size{};
    VmaFlags m_vmaf{};
    MmuFlags m_mmuf{};
    VmMapOption m_option{};
    VmArea *m_parent{};
    ustl::Arc<VmAspace> *m_aspace{};
    char const *m_name{};
    usize m_vmo_ofs{};
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
    FORCE_INLINE
    auto empty() const -> bool {
        return m_tree.empty();
    }

    [[nodiscard]]
    FORCE_INLINE
    auto size() const -> usize {
        return m_tree.size();
    }

    FORCE_INLINE
    auto begin() -> IterMut {
        return m_tree.begin();
    }
    FORCE_INLINE
    auto begin() const -> Iter {
        return m_tree.begin();
    }
    FORCE_INLINE
    auto end() -> IterMut {
        return m_tree.end();
    }
    FORCE_INLINE
    auto end() const -> Iter {
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
    /// lower and may still reach as far as |addr|. Anything before *that* ends
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
    FORCE_INLINE
    auto lower_bound(VirtAddr addr) -> ustl::Option<IterMut> {
        return m_tree.lower_bound(addr);
    }
    FORCE_INLINE
    auto lower_bound(VirtAddr addr) const -> ustl::Option<Iter> {
        return m_tree.lower_bound(addr);
    }

    /// Find the first existent VMA whose base > addr.
    FORCE_INLINE
    auto upper_bound(VirtAddr addr) -> ustl::Option<IterMut> {
        return m_tree.upper_bound(addr);
    }
    FORCE_INLINE
    auto upper_bound(VirtAddr addr) const -> ustl::Option<Iter> {
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
    /// The window is [first, last], both ends inclusive, because an exclusive
    /// one cannot name a window that runs to the top of the address space --
    /// see VmAreaOrMapping::last(). Dumping or tearing down a whole VmArea is
    /// exactly that window on the kernel root VMA.
    Enumerator(VmaSet &, VirtAddr first, VirtAddr last);
    Enumerator(VmArea &, VirtAddr first, VirtAddr last);
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
    VirtAddr m_last;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_AREA_OR_MAPPING_HPP
