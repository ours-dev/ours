/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_FRAME_HPP
#define OURS_MM_PM_FRAME_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/types.hpp>

#include <atomic>
#include <type_traits>

#include <ustl/bit.hpp>
#include <ustl/bitfields.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/mem/lifetime.hpp>
#include <ustl/option.hpp>
#include <ustl/traits/is_base_of.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
namespace page {
    enum class Role {
        None,
        Io,
        Lru,
        Vmm,
        Pmm,
        Slab,
        Heap,
        Folio,

        /// A hardware page-table page. It belongs to whichever ArchVmAspace
        /// built it, is walked by the CPU rather than read by the kernel, and
        /// must never move or be reclaimed while any root points at it.
        ///
        /// Zircon marks the same frames vm_page_state::MMU, both at
        /// ArmArchVmAspace::AllocPageTable() and for the tables physboot built
        /// (PmmNode::InitReservedRange() hands those to
        /// ArchVmAspace::HandoffPageTablesFromPhysboot()).
        Mmu,

        Count,
    };

    enum class Flags {
        Active = BIT(0),
        Pinned = BIT(1),
        Dirty = BIT(2),
        UpToDate = BIT(3),
        Foreign = BIT(4),
        BeginWaiting = BIT(5),
        Reclaimable = BIT(7),
        Reserved = BIT(8),

        Initialized = BIT(9),
        Count,
    };
    USTL_ENABLE_ENUM_BITMASK(Flags);

    enum class Index {
        Role,
        Flags,
        Order,
        Node,
        Section,
        ZoneType,
    };

    template <Index Index, typename Type, usize Bits, bool Enabled = true>
    using Field =
        ustl::bitfields::Field<ustl::bitfields::Id<Index>, ustl::bitfields::Type<Type>,
                               ustl::bitfields::Bits<Bits>, ustl::bitfields::Enable<Enabled>>;

    using Status = ustl::BitFields<
        Field<Index::Role, Role, ustl::bit_width(static_cast<usize>(Role::Count))>,
        Field<Index::Flags, Flags, ustl::bit_width(static_cast<usize>(Flags::Count))>,
        Field<Index::Order, usize, ustl::bit_width(static_cast<usize>(MAX_FRAME_ORDER))>,
        // Readonly after initialized
        Field<Index::ZoneType, ZoneType, ustl::bit_width(static_cast<usize>(ZoneType::Count))>,
        // Readonly after initialized
        Field<Index::Section, SecNum, MAX_PHYSADDR_BITS - SECTION_SIZE_BITS>,
        // Readonly after initialized
        Field<Index::Node, NnId, NUMA_MAX_NODES_BITS, OURS_CONFIG_NUMA>>;

    /// Per-role payload. Exactly one of these is live at a time, selected by
    /// `Index::Role` in the Status word above.
    ///
    /// They are deliberately plain data: no constructors, no destructors, no
    /// intrusive *containers*. A role change is then just "zero the union and
    /// set the tag", with no lifetime bookkeeping to get wrong. The
    /// static_asserts at the bottom of this header are what keep it that way --
    /// an intrusive container's header has to point at itself when empty, which
    /// zeroed bytes cannot express, so admitting one here would silently break
    /// every role transition.
    ///
    /// The list hook is *not* here. It lives in PageFrame, because a frame is
    /// on at most one list at a time whatever its role: free (buddy), in use by
    /// the VMM (page queues), or backing a slab (partial list) are mutually
    /// exclusive. One hook also sidesteps a language limitation -- a
    /// pointer-to-member cannot reach into a union member, so intrusive list
    /// traits could not name a hook nested in here.

    struct PmmBody {
        constexpr static auto ROLE = Role::Pmm;
    };

    struct VmmBody {
        constexpr static auto ROLE = Role::Vmm;

        /// Opaque on purpose: the owner is a VmObject for some pages and a
        /// VmCowPages for others, and the two are unrelated types. Callers know
        /// which one they put here and cast it back themselves.
        void *vmo;
        std::atomic<u32> vmo_index;    // Index in VMO's page map
        std::atomic<u16> num_mappings; // 65536 mappings
        std::atomic<u16> num_users;    // 65536 users
        std::atomic<u8> pinned_count;

        /// How many more than one of the copy-on-write tree's leaves can see
        /// this page: zero for a page that has one viewer, and only ever above
        /// zero for a page held by a hidden node, which is what makes it shared.
        /// It says whether a writer may take the page (zero: it is the only
        /// one who sees it) or has to copy it (above zero: someone else does).
        ///
        /// Plain, not atomic: it is read and written only under the lock of the
        /// tree whose node holds the page, which is the same lock that decides
        /// who sees the page. Zircon: vm_page_t::object.share_count.
        u32 share_count;

        /// Which of PageQueues' lists currently holds this frame, as a
        /// PageQueues::QueueType. Zero is that enum's `None`, so a frame that
        /// has just become() a VMM page reads as unqueued without anyone
        /// having to say so.
        ///
        /// It has to live here rather than be derived: the descriptor carries
        /// one list hook for every role (see the comment above), so re-queuing
        /// a page means detaching it from the list it is on -- and an
        /// intrusive node cannot name its own container. Zircon keeps the same
        /// byte for the same reason, as `vm_page_t::object.page_queue`.
        ///
        /// Plain rather than atomic: every read and write of it happens under
        /// the owning PageQueues' spin lock, and making it atomic would only
        /// advertise a lock-free protocol that does not exist here.
        u8 queue;
    };

    struct SlabBody {
        constexpr static auto ROLE = Role::Slab;

        /// Null-terminated free list, not an intrusive container: an empty
        /// slab is then simply a null pointer, which zeroed storage already
        /// says. (A circular list would need a self-pointing header instead.)
        /// This also matches how Linux's slub tracks its freelist.
        void *free_head;
        void *owner; // MemCache *, opaque here to avoid the include cycle.

        /// MemCache::NodeCache *, opaque here for the same reason.
        ///
        /// Recorded rather than looked up from |owner| and the frame's node,
        /// because the buddy may satisfy a request from a fallback node that
        /// the cache has no NodeCache for -- and free() has to know, without
        /// any doubt, which lock protects this slab.
        void *node_cache;

        u16 num_inuse;
        u16 num_objects;

        /// Byte offset, within an object, at which the free-list pointer is
        /// stored. Copied from the owning cache's computed layout so the slab
        /// routines never have to reach back into MemCache for it -- they are
        /// free functions and cannot see its private members.
        u16 next_offset;
    };

} // namespace page

/// The descriptor for one physical frame.
///
/// There is exactly one of these per frame, and it plays different roles over
/// its life. The fields above the union survive every role change -- in
/// particular ZoneType/Section/Node are written once at memmap setup and are
/// read-only afterwards. Only the union changes, and only through become().
///
/// `VmPage`, `PmFrame` and `Slab` are aliases of this type (see below). They
/// carry intent in signatures; the role is enforced at runtime by the
/// DEBUG_ASSERTs on the role-specific accessors.
class PageFrame {
    using Self = PageFrame;

  public:
    using Role = page::Role;
    using Flags = page::Flags;

    /// Default-constructible so PmFrame arrays (kernel memmap and host
    /// tests) can be allocated before init() fills each descriptor.
    PageFrame() = default;

    constexpr static auto SIZE_LIMIT = sizeof(usize) * 8;

    /// What is left for the role payload once the fields every role shares
    /// have taken their share. Named so the payload budget is stated once.
    constexpr static auto BODY_SIZE = SIZE_LIMIT - 32;

    constexpr static auto ALIGNMENT = sizeof(usize) * 8;

    auto init(ZoneType ztype, SecNum secnum, NnId nid) -> void {
        DEBUG_ASSERT(!(flags() & Flags::Initialized), "Duplicated to initialize a frame");

        m_status.set<Index::ZoneType>(ztype);
        m_status.set<Index::Section>(secnum);
        m_status.set<Index::Node>(nid);
        m_status.set<Index::Role>(Role::None);
        set_flags(Flags::Active | Flags::UpToDate | Flags::Initialized);
    }

    FORCE_INLINE
    constexpr auto nid() const -> NnId {
        return m_status.get<Index::Node>();
    }

    FORCE_INLINE
    constexpr auto zone() const -> ZoneType {
        return m_status.get<Index::ZoneType>();
    }

    FORCE_INLINE
    constexpr auto secnum() const -> SecNum {
        return m_status.get<Index::Section>();
    }

    FORCE_INLINE
    constexpr auto order() const -> usize {
        return m_status.get<Index::Order>();
    }

    FORCE_INLINE
    constexpr auto set_order(usize order) -> void {
        m_status.set<Index::Order>(order);
    }

    FORCE_INLINE
    constexpr auto mark_pinned() -> void {
        set_flags(Flags::Pinned);
    }

    FORCE_INLINE
    constexpr auto mark_reserved() -> void {
        set_flags(Flags::Reserved);
    }

    FORCE_INLINE
    constexpr auto mark_active() -> void {
        set_flags(Flags::Active);
    }

    FORCE_INLINE
    constexpr auto role() const -> Role {
        return m_status.get<Index::Role>();
    }

    FORCE_INLINE
    constexpr auto set_role(Role role) -> void {
        m_status.set<Index::Role>(role);
    }

    FORCE_INLINE
    constexpr auto is_role(Role role) const -> bool {
        return this->role() == role;
    }

    FORCE_INLINE
    constexpr auto phys_size() const -> usize {
        return BIT(order()) << PAGE_SHIFT;
    }

    FORCE_INLINE
    constexpr auto flags() const -> Flags {
        return m_status.get<Index::Flags>();
    }

    auto dump() const -> void;

    /// Detaches this frame from whatever list holds it. AutoUnlink, so the
    /// caller does not need to know which one.
    auto unlink() -> void {
        m_hook.unlink();
    }

    [[nodiscard]]
    auto is_linked() const -> bool {
        return m_hook.is_linked();
    }

    /// How far this frame sits from the head of the multi-frame block it
    /// belongs to, in frames. Zero for a head frame and for every block of a
    /// single frame, which is what zeroed memmap storage already says.
    ///
    /// This is what lets `frame_to_folio` walk from an interior address back
    /// to the descriptor that carries the block's metadata. Whoever claims a
    /// multi-frame block is responsible for stamping the tails and for
    /// clearing them again before handing the block back -- see slab::create
    /// and slab::destroy, the only pair doing so today.
    FORCE_INLINE
    auto compound_offset() const -> u32 {
        return m_compound.load(std::memory_order_relaxed);
    }

    FORCE_INLINE
    auto set_compound_offset(u32 offset) -> void {
        m_compound.store(offset, std::memory_order_relaxed);
    }

    auto get() const -> void {
        m_references.fetch_add(1, std::memory_order_relaxed);
    }
    auto put() const -> void {
        m_references.fetch_sub(1, std::memory_order_relaxed);
    }

    /// Takes on |Body|'s role: clears the payload and stamps the tag.
    ///
    /// This is the only thing that writes the role. Reading a frame -- taking
    /// its address, dumping it, asking what it is -- must never change it.
    template <typename Body>
    FORCE_INLINE
    auto become() -> Body & {
        static_assert(sizeof(Body) <= sizeof(m_body), "role payload is too large");

        for (auto &byte: m_body) {
            byte = std::byte{};
        }
        m_status.set<Index::Role>(Body::ROLE);
        return *ustl::mem::start_lifetime_as<Body>(&m_body);
    }

    /// Interprets this frame as |Body|, or null if it currently plays a
    /// different role. For callers that start from a pointer of unknown
    /// provenance -- kfree reaching a frame from an arbitrary object address.
    template <typename Body>
    FORCE_INLINE
    auto try_as() -> Body * {
        return role() == Body::ROLE ? std::launder(reinterpret_cast<Body *>(&m_body)) : nullptr;
    }

    /// Interprets this frame as |Body| where the role is already known from
    /// context. Checked in debug builds.
    template <typename Body>
    FORCE_INLINE
    auto as() -> Body & {
        DEBUG_ASSERT(role() == Body::ROLE, "frame plays another role");
        return *std::launder(reinterpret_cast<Body *>(&m_body));
    }

    template <typename Body>
    FORCE_INLINE
    auto as() const -> Body const & {
        DEBUG_ASSERT(role() == Body::ROLE, "frame plays another role");
        return *std::launder(reinterpret_cast<Body const *>(&m_body));
    }

    // ---- VMM role -------------------------------------------------------

    auto set_object(void *object) -> void {
        as<page::VmmBody>().vmo = object;
    }

    auto get_object() -> void * {
        return as<page::VmmBody>().vmo;
    }

    auto set_pgoff(PgOff pgoff) -> void {
        as<page::VmmBody>().vmo_index.store(static_cast<u32>(pgoff));
    }

    auto get_pgoff() const -> PgOff {
        return static_cast<PgOff>(as<page::VmmBody>().vmo_index.load());
    }

    auto increase_mappings(usize num = 1) -> void {
        as<page::VmmBody>().num_mappings.fetch_add(static_cast<u16>(num));
    }

    auto decrease_mappings(usize num = 1) -> void {
        as<page::VmmBody>().num_mappings.fetch_sub(static_cast<u16>(num));
    }

    auto num_mappings() const -> usize {
        return as<page::VmmBody>().num_mappings.load();
    }

    /// See VmmBody::share_count. The caller holds the lock of the tree whose
    /// node holds this page.
    auto share_count() const -> u32 {
        return as<page::VmmBody>().share_count;
    }

    auto add_share() -> void {
        as<page::VmmBody>().share_count += 1;
    }

    /// The PageQueues list this frame is filed on, as the raw enumerator
    /// value -- PageQueues::QueueType is private to that class, so the
    /// descriptor carries the byte and leaves its meaning there.
    auto page_queue() const -> u8 {
        return as<page::VmmBody>().queue;
    }

    auto set_page_queue(u8 queue) -> void {
        as<page::VmmBody>().queue = queue;
    }

    // ---- Slab role ------------------------------------------------------

    auto slab_owner() const -> void * {
        return as<page::SlabBody>().owner;
    }

    auto set_slab_owner(void *owner) -> void {
        as<page::SlabBody>().owner = owner;
    }

  protected:
    using Index = page::Index;

    USTL_NO_MOVEABLE_AND_COPYABLE(PageFrame);

    FORCE_INLINE
    constexpr auto set_flags(Flags flags) -> void {
        m_status.set<Index::Flags>(flags);
    }

  private:
    page::Status m_status;

    // The size of following fields type should be half of a word.
    mutable std::atomic<u32> m_references;

    // Trace the position of current frame in a large frame.
    mutable std::atomic<u32> m_compound;

    /// AutoUnlink because Slab::destroy detaches a frame without knowing which
    /// list holds it. One hook serves every role -- see page::PmmBody above.
    using AutoUnlinkMode = ustl::collections::intrusive::LinkMode<
        ustl::collections::intrusive::LinkModeType::AutoUnlink>;

    ustl::collections::intrusive::ListMemberHook<AutoUnlinkMode> m_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_hook, ManagedOption);

    /// The role payload. Raw storage rather than a union of the body types so
    /// that become() can start a lifetime in it without the union's own
    /// special-member rules getting in the way.
    /// Sized so the whole descriptor lands exactly on SIZE_LIMIT; the
    /// static_assert below is what actually enforces that.
    alignas(sizeof(usize)) std::byte m_body[BODY_SIZE];

  public:
    USTL_DECLARE_LIST(Self, List, ManagedOption,
                      ustl::collections::intrusive::ConstantTimeSize<false>);
};

/// Role aliases. They document intent in signatures; the role itself is
/// checked at runtime by the accessors above. The previous design made these
/// distinct classes and converted with static_cast, which was undefined (no
/// object of the target type ever existed) and, worse, stamped the role tag as
/// a side effect of converting -- so asking a frame what it was changed it.

/// A folio's overflow metadata lives in the *next* descriptor of the memmap,
/// reached by neighbour addressing rather than by a member -- a member would be
/// packed into this descriptor's own padding and never reach the next frame.
/// TODO(SmallHuaZi): unused so far; large-page support will need it, along
/// with a bounds check against the end of the section's memmap.

static_assert(sizeof(PageFrame) == PageFrame::SIZE_LIMIT,
              "the memmap stride is what the role overlay depends on");
static_assert(alignof(PageFrame) <= PageFrame::ALIGNMENT);

/// Role payloads have to fit the shared budget. become() checks this too, but
/// only for the roles some translation unit happens to instantiate -- here it
/// is checked for all of them, so adding a field to a body that is briefly
/// unused still fails the build rather than later, somewhere else.
///
/// SlabBody is currently at the limit exactly: it needs one more field and the
/// descriptor has to grow, or something else has to give.
static_assert(sizeof(page::PmmBody) <= PageFrame::BODY_SIZE);
static_assert(sizeof(page::VmmBody) <= PageFrame::BODY_SIZE);
static_assert(sizeof(page::SlabBody) <= PageFrame::BODY_SIZE);

/// Role payloads must stay plain data: see page::PmmBody's comment.
static_assert(std::is_trivially_copyable_v<page::PmmBody> &&
              std::is_trivially_destructible_v<page::PmmBody>);
static_assert(std::is_trivially_copyable_v<page::VmmBody> &&
              std::is_trivially_destructible_v<page::VmmBody>);
static_assert(std::is_trivially_copyable_v<page::SlabBody> &&
              std::is_trivially_destructible_v<page::SlabBody>);

auto to_string(PageFrame::Role role) -> char const *;

auto to_string(ZoneType zone) -> char const *;

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_FRAME_HPP
