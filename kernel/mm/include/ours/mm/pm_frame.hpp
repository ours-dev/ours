/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_FRAME_HPP
#define OURS_MM_PM_FRAME_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/types.hpp>

#include <atomic>
#include <type_traits>
#include <utility>

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
    /// What a frame is used for now, and so which payload of PageFrame is
    /// live: one value at a time, rewritten whenever the frame changes hands
    /// (become(), the buddy). A use, not a kind of page: whether a frame was
    /// held since boot is not a use but a fact about it whatever its use, so
    /// it is the Reserved flag below, not a role. A boot page table is
    /// therefore Mmu and Reserved at once, and a frame the boot chain holds
    /// with no mm-side owner (the kernel image, the boot image) is None and
    /// Reserved.
    ///
    /// Only the roles something writes are here. Io, Lru, Heap and Folio went
    /// because nothing ever became them: LRU membership is a page queue, as
    /// for any Vmm page; kmalloc pages are Slab and vmalloc pages Vmm; and a
    /// compound page is the m_compound stamp, which a Slab of order above 0
    /// carries too, so it cannot be an exclusive role.
    enum class Role : u32 {
        /// Allocated, with no owner yet: what the buddy hands out and what
        /// init() leaves, so also a reserved boot frame other than a page
        /// table, until unwire_frame() hands it on.
        None,
        Vmm,

        /// On a buddy free list. Only FrameBuddy::insert_frame() writes it,
        /// under the zone lock, and FrameBuddy::is_mergeable() reads it on a
        /// neighbour as "free, so coalesce with it". A frame that says Pmm
        /// while on no list gets coalesced anyway: a dying slab that said it
        /// early trapped in FrameBuddy::release(), and frame_to_phys()
        /// once said it of live frames, which were then handed out twice.
        /// That is why it has no body type: become() would let any caller
        /// write it.
        Pmm,
        Slab,

        /// A hardware page-table page. It belongs to whichever ArchVmAspace
        /// built it, is walked by the CPU rather than read by the kernel, and
        /// must never move or be reclaimed while any root points at it.
        /// stamp_reserved_frames() sets it on the tables physboot built, and
        /// alloc_page_table() on every table the buddy gives out;
        /// free_page_table() asserts it.
        Mmu,

        Count,
    };

    /// What a frame's role does not say. The flags this once carried (active,
    /// dirty, uptodate, foreign, reclaim and a waiters bit) stood for an LRU, a
    /// page cache, a hypervisor's foreign pages and reclaim, none of which this
    /// kernel has, and nothing read them. Whether a page is active or
    /// reclaimable is its page-queue membership (PageQueues), not a bit here.
    ///
    /// Each bit has its own writer and the others are left alone: there is no
    /// setter that assigns the whole field, because the one there was made
    /// every mark erase the rest. That is how page tables lost Reserved.
    enum class Flags : u32 {
        /// Held since boot, not the allocator's: set when the boot chain's
        /// reservations are stamped and on holes, cleared by unwire_frame()
        /// when a frame leaves the reserved books. A bit, not a role, so that
        /// a boot page table can be Mmu and Reserved at once; see Role.
        Reserved = 0_bit,

        /// The descriptor has been initialised. Set once, by init().
        Initialized = 1_bit,
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

    // cfg.hpp spells NUMA_MAX_NODES_BITS out by hand, because assembly reads it
    // too; this is what keeps it equal to the width of NUMA_MAX_NODE.
    static_assert(NUMA_MAX_NODES_BITS == ustl::bit_width(static_cast<usize>(NUMA_MAX_NODE)));

    using Status = ustl::BitFields<
        Field<Index::Role, Role, ustl::bit_width(std::to_underlying(Role::Count))>,
        Field<Index::Flags, Flags, ustl::bit_width(std::to_underlying(Flags::Count))>,
        Field<Index::Order, usize, ustl::bit_width(static_cast<usize>(MAX_FRAME_ORDER))>,
        // Readonly after initialized
        Field<Index::ZoneType, ZoneType, ustl::bit_width(std::to_underlying(ZoneType::Count))>,
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
    ///
    /// None and Pmm carry no payload, so they have no body. For Pmm that is
    /// also a guard: see the role's own comment.

    struct VmmBody {
        constexpr static auto ROLE = Role::Vmm;

        /// Opaque on purpose: the owner is a VmObject for some pages and a
        /// VmCowPages for others, and the two are unrelated types. Callers know
        /// which one they put here and cast it back themselves.
        void *vmo;
        std::atomic<u32> vmo_index;    // Index in VMO's page list
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
        /// who sees the page.
        u32 share_count;

        /// Which of PageQueues' lists currently holds this frame, as a
        /// PageQueues::QueueType. Zero is that enum's `None`, so a frame that
        /// has just become() a VMM page reads as unqueued without anyone
        /// having to say so.
        ///
        /// It has to live here rather than be derived: the descriptor carries
        /// one list hook for every role (see the comment above), so re-queuing
        /// a page means detaching it from the list it is on -- and an
        /// intrusive node cannot name its own container.
        ///
        /// Plain rather than atomic: every read and write of it happens under
        /// the owning PageQueues' spin lock, and making it atomic would only
        /// advertise a lock-free protocol that does not exist here.
        u8 queue;
    };

    /// A page-table page's: the tables set aside for its large leaves.
    ///
    /// A second-level table of an address space whose mappings change in part
    /// (a user one) keeps, for every 2 MiB leaf it holds, one zeroed table to
    /// split that leaf into (PageTableDeposits, decision D4 of
    /// .agents/tasks/large-pages). They are a chain through these bodies:
    /// |deposits| on the table that holds the leaves names the first, and
    /// |next_deposit| on each table set aside names the one after it. A chain of
    /// bare pointers rather than the descriptor's list hook, because the head
    /// lives in a body, and a zeroed body cannot be an empty intrusive list
    /// (see the comment above VmmBody). Both null for every other table.
    ///
    /// Plain: written only under the lock of the page table they belong to.
    struct MmuBody {
        constexpr static auto ROLE = Role::Mmu;

        PageFrame *deposits;
        PageFrame *next_deposit;
    };

    struct SlabBody {
        constexpr static auto ROLE = Role::Slab;

        /// Null-terminated free list, not an intrusive container: an empty
        /// slab is then simply a null pointer, which zeroed storage already
        /// says. (A circular list would need a self-pointing header instead.)
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
        // The one write of the whole field: whatever the descriptor held
        // before is garbage.
        m_status.set<Index::Flags>(Flags::Initialized);
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

    /// Sets Reserved and leaves the other flags alone. It counts nothing:
    /// stamp_reserved_frames() adds the frames it marks to the node's and
    /// zone's reserved counts, and a hole is marked but never counted. A
    /// frame is marked once. The stamp skips one already Reserved, so a
    /// second mark would be a frame the stamp counted twice.
    FORCE_INLINE
    auto mark_reserved() -> void {
        DEBUG_ASSERT(!(flags() & Flags::Reserved), "frame reserved twice");
        m_status.set<Index::Flags>(flags() | Flags::Reserved);
    }

    /// Clears Reserved: one half of taking a frame off the reserved books, which
    /// unwire_frame() does for every frame that leaves them (the init area
    /// freed after boot, the boot image adopted by userboot's VMO). It comes
    /// before the frame is freed: PmNode::free_frames() asserts that the
    /// frame it is given is not reserved.
    FORCE_INLINE
    constexpr auto clear_reserved() -> void {
        m_status.set<Index::Flags>(flags() & ~Flags::Reserved);
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
        return (usize{1} << order()) << PAGE_SHIFT;
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
    /// This is the only thing that writes the role and the payload together;
    /// the buddy (insert_frame(), remove_frame()) and the boot stamp write
    /// the role alone, through set_role(). Reading a frame -- taking its
    /// address, dumping it, asking what it is -- must never change it.
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

    /// One viewer fewer. The count is unsigned, so a drop of zero would not go
    /// negative but wrap to four billion, and no writer would ever see zero
    /// again: the page would be stuck in the hidden node, copied by every
    /// writer and taken by none. It is an assertion for that reason.
    auto drop_share() -> void {
        auto &body = as<page::VmmBody>();
        DEBUG_ASSERT(body.share_count > 0, "a page with no other viewer lost one");
        body.share_count -= 1;
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

  private:
    page::Status m_status;

    // The size of following fields type should be half of a word.
    mutable std::atomic<u32> m_references;

    // Trace the position of current frame in a large frame.
    mutable std::atomic<u32> m_compound;

    /// AutoUnlink because Slab::destroy detaches a frame without knowing which
    /// list holds it. One hook serves every role -- see the comment above
    /// page::VmmBody.
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
static_assert(sizeof(page::VmmBody) <= PageFrame::BODY_SIZE);
static_assert(sizeof(page::MmuBody) <= PageFrame::BODY_SIZE);
static_assert(sizeof(page::SlabBody) <= PageFrame::BODY_SIZE);

/// Role payloads must stay plain data: see the comment above page::VmmBody.
static_assert(std::is_trivially_copyable_v<page::VmmBody> &&
              std::is_trivially_destructible_v<page::VmmBody>);
static_assert(std::is_trivially_copyable_v<page::MmuBody> &&
              std::is_trivially_destructible_v<page::MmuBody>);
static_assert(std::is_trivially_copyable_v<page::SlabBody> &&
              std::is_trivially_destructible_v<page::SlabBody>);

auto to_string(PageFrame::Role role) -> char const *;

auto to_string(ZoneType zone) -> char const *;

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_FRAME_HPP
