/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_FRAME_HPP
#define OURS_MM_PM_FRAME_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/types.hpp>
#include <ours/node_mask.hpp>
#include <ours/status.hpp>

#include <atomic>
#include <limits>
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
    /// compound page is the m_compound stamp, which every allocated block
    /// above order 0 carries, whatever its role, so it cannot be a role.
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

    // MAX_NODES_BITS used to be a second #define per NUMA branch, written out
    // beside the count, and the static_assert here was what caught the two
    // drifting apart. <ours/node_mask.hpp> computes it from the count now, so
    // there is nothing left for an assertion to check.

    /// The frame's packed status: role, flags, block order, zone, section and
    /// node in one word. StatusWord rather than Status, which inside page::
    /// would hide ours::Status, the result type every mm operation returns.
    using StatusWord = ustl::BitFields<
        Field<Index::Role, Role, ustl::bit_width(std::to_underlying(Role::Count))>,
        Field<Index::Flags, Flags, ustl::bit_width(std::to_underlying(Flags::Count))>,
        Field<Index::Order, usize, ustl::bit_width(static_cast<usize>(MAX_FRAME_ORDER))>,
        // Readonly after initialized
        Field<Index::ZoneType, ZoneType, ustl::bit_width(std::to_underlying(ZoneType::Count))>,
        // Readonly after initialized
        Field<Index::Section, SecNum, MAX_PHYSADDR_BITS - SECTION_SIZE_BITS>,
        // Readonly after initialized
        Field<Index::Node, NodeId, MAX_NODES_BITS, OURS_CONFIG_NUMA>>;

    /// Per-role payload. Exactly one of these is live at a time, selected by
    /// `Index::Role` in the status word above.
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

class FrameBuddy;
class PmZone;

namespace priv {
    class MemmapInit;
    class Pipeline;
} // namespace priv

/// A pass to a frame's Reserved flag (PageFrame::mark_reserved(),
/// clear_reserved()). A frame wired into the reserved books is the flag and
/// one count in its zone, and PmZone::wire() and unwire() are what write the
/// two together, so the zone makes the key. So does what marks a frame
/// reserved with no zone to count it in: memmap's bring-up, for a hole and for
/// a reservation whose node has no zone of the frame's type, and the
/// pipeline, which unwires such a frame (mm::unwire_frame()).
class WireKey final {
    friend class PmZone;
    friend class priv::MemmapInit;
    friend class priv::Pipeline;
    WireKey() = default;
};

/// What a frame asks to be moved onto or off a buddy free list: a passkey only
/// FrameBuddy can make, so only it can write Role::Pmm. A friend declaration
/// would open every private member of the frame to the buddy; this opens two
/// functions, and only to the one caller that may use them.
class FrameBuddyKey {
    friend class FrameBuddy;
    FrameBuddyKey() = default;
};

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

    auto init(ZoneType ztype, SecNum secnum, NodeId nid) -> void {
        DEBUG_ASSERT(!(flags() & Flags::Initialized), "Duplicated to initialize a frame");

        // Built whole and stored once: nothing of what the storage held
        // before survives.
        auto word = page::StatusWord{};
        word.set<Index::ZoneType>(ztype);
        word.set<Index::Section>(secnum);
        word.set<Index::Node>(nid);
        word.set<Index::Role>(Role::None);
        // No block yet: a frame is the head of a block only once the buddy
        // has made one of it, and the buddy writes the order then. Written
        // here all the same, because free_frames() reads it to learn how much
        // is being returned, and a frame nothing has allocated must read as a
        // single frame and not as whatever its storage held.
        word.set<Index::Order>(0);
        word.set<Index::Flags>(Flags::Initialized);
        m_status.store(word.into<page::StatusWord::StorageUnit>(), std::memory_order_relaxed);
    }

    FORCE_INLINE
    auto nid() const -> NodeId {
        return status().get<Index::Node>();
    }

    FORCE_INLINE
    auto zone() const -> ZoneType {
        return status().get<Index::ZoneType>();
    }

    FORCE_INLINE
    auto secnum() const -> SecNum {
        return status().get<Index::Section>();
    }

    FORCE_INLINE
    auto order() const -> usize {
        return status().get<Index::Order>();
    }

    /// The order the buddy hands a block out at, written on its head as it
    /// leaves the free list. Anyone else changes a block's layout through
    /// make_block() or split(), which stamp the tails with it.
    FORCE_INLINE
    auto set_order(usize order, FrameBuddyKey) -> void {
        set_status<Index::Order>(order);
    }

    /// Sets Reserved and leaves the other flags alone. It counts nothing:
    /// PmZone::wire() counts the frame in its zone along with the mark, and
    /// two kinds are marked but never counted (WireKey): a hole, and a
    /// reservation whose node has no zone of the frame's type. A frame is
    /// marked once. The stamp skips one already Reserved, so a second mark
    /// would be a frame the stamp counted twice.
    FORCE_INLINE
    auto mark_reserved(WireKey) -> void {
        DEBUG_ASSERT(!(flags() & Flags::Reserved), "frame reserved twice");
        set_status<Index::Flags>(flags() | Flags::Reserved);
    }

    /// Clears Reserved: the frame's half of taking it off the reserved books,
    /// which every frame leaving them does through mm::unwire_frame() (the
    /// init area freed after boot, the boot image adopted by userboot's VMO):
    /// PmZone::unwire() clears it with the count, and a frame no zone counted
    /// has the flag cleared alone. It comes before the frame is freed:
    /// mm::free_frames() asserts that the frame it is given is not reserved.
    FORCE_INLINE
    auto clear_reserved(WireKey) -> void {
        set_status<Index::Flags>(flags() & ~Flags::Reserved);
    }

    FORCE_INLINE
    auto role() const -> Role {
        return status().get<Index::Role>();
    }

    /// Puts the frame on a free list as the head of a block of 2^|order|:
    /// Role::Pmm and the order, in one store. FrameBuddy::insert_frame()
    /// alone can call it, under the zone lock.
    FORCE_INLINE
    auto enter_free_list(usize order, FrameBuddyKey) -> void {
        auto word = status();
        word.set<Index::Role>(Role::Pmm);
        word.set<Index::Order>(order);
        m_status.store(word.into<page::StatusWord::StorageUnit>(), std::memory_order_relaxed);
    }

    /// Takes the frame off a free list: Role::None, the role of a frame
    /// nobody has made anything of. FrameBuddy::remove_frame() alone can call
    /// it, under the zone lock.
    FORCE_INLINE
    auto leave_free_list(FrameBuddyKey) -> void {
        DEBUG_ASSERT(role() == Role::Pmm, "frame {:p} leaves a free list it was not on",
                     static_cast<void const *>(this));
        set_status<Index::Role>(Role::None);
    }

    /// Writes a role and sets up no body: None, or the role a reserved boot
    /// frame plays, stamped before the buddy exists (memmap_init stamps a
    /// boot page table Mmu, its payload left as the memmap storage holds
    /// it, zeroed). Never Pmm, which only the buddy writes (enter_free_list()):
    /// a frame that says Pmm while on no list is coalesced as though it were
    /// free. A role whose body must be set up is become()'s.
    FORCE_INLINE
    auto stamp_role(Role role) -> void {
        ASSERT(role != Role::Pmm, "frame {:p} stamped Pmm by hand",
               static_cast<void const *>(this));
        set_status<Index::Role>(role);
    }

    FORCE_INLINE
    auto is_role(Role role) const -> bool {
        return this->role() == role;
    }

    FORCE_INLINE
    auto phys_size() const -> usize {
        return (usize{1} << order()) << PAGE_SHIFT;
    }

    FORCE_INLINE
    auto flags() const -> Flags {
        return status().get<Index::Flags>();
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
    /// to the descriptor that carries the block's metadata. The allocator
    /// stamps the tails of every block it hands out and clears them when the
    /// block comes back (make_block(), clear_block()); split() re-stamps.
    FORCE_INLINE
    auto compound_offset() const -> u32 {
        return m_compound.load(std::memory_order_relaxed);
    }

    /// Whether this frame heads a block rather than sitting inside one. Every
    /// tail of a block the allocator handed out says how far it is from its
    /// head, so a frame that says 0 is a head (or a block of one frame).
    [[nodiscard]]
    FORCE_INLINE
    auto is_head() const -> bool {
        return compound_offset() == 0;
    }

    /// Makes this frame the head of an allocated block of 2^|order| frames:
    /// its order, and every tail's distance from it, so that any frame of the
    /// block leads back to the head and only the head reads as one.
    /// prepare_alloc() calls it for every block it hands out; one store per
    /// frame, less than touching the block's memory once.
    ///
    /// The tails are reached by indexing from the head: a block never
    /// crosses a section of the memmap, so its descriptors are consecutive.
    auto make_block(usize const order) -> void {
        set_status<Index::Order>(order);
        stamp_tails(0, usize{1} << order);
    }

    /// Splits the block this frame heads into blocks of 2^|order| frames,
    /// each a head of its own order with its tails stamped from it. The
    /// caller owns the block; nothing else reads it while it is on no list.
    auto split(usize const order) -> void {
        auto const whole = this->order();
        DEBUG_ASSERT(order <= whole, "a block of order {} cannot be split into blocks of order {}",
                     whole, order);
        auto const step = usize{1} << order;
        for (usize head = 0; head < (usize{1} << whole); head += step) {
            this[head].set_status<Index::Order>(order);
            stamp_tails(head, step);
        }
    }

    /// Undoes make_block() before the block goes back to a free list: every
    /// tail's offset back to 0, as a frame on no block says. The buddy may hand
    /// these frames out separately next time.
    auto clear_block() -> void {
        auto const nr_frames = usize{1} << order();
        for (usize i = 1; i < nr_frames; ++i) {
            this[i].set_compound_offset(0);
        }
    }

    /// How many pins hold this frame in place. A frame with any must keep its
    /// physical address and its content: it cannot be reclaimed, replaced,
    /// zero-deduplicated, taken out of its node, or shared with a clone,
    /// because something other than the CPU, a device that does DMA to it,
    /// knows where it is.
    ///
    /// A fact about the frame whatever its role, which is why it is here and
    /// not in a role's body: a change of role does not forget a pin, and the
    /// allocator can check, on the way into stock and out of it, that no
    /// frame is free while held.
    ///
    /// Written only under the lock of the frame's owner (for a page of a VMO,
    /// the lock of the node that holds it), so a pin and an unpin never race
    /// each other. Movers do not read it directly: the owner files a frame
    /// with any pin where its movers will not take it, on the first pin and
    /// off on the last (see mm::pin_frame()). Atomic only so that a reader
    /// without the lock, a dump or a test, reads a whole value.
    [[nodiscard]]
    FORCE_INLINE
    auto pin_count() const -> usize {
        return m_pins.load(std::memory_order_relaxed);
    }

    /// One more pin, and every rule a pin has, here with the count they
    /// guard (mm::pin_frame() says what the caller must hold). BadState for
    /// the head of a free block, which nobody owns, so nobody may pin;
    /// NoResource when the count is at its limit (255), where a pin has to be
    /// refused rather than wrapped to none. Nothing changes when it refuses.
    /// The caller holds the owner's lock, so the read and the write are not
    /// one atomic step and do not need to be.
    [[nodiscard]]
    auto pin() -> Status {
        if (is_role(Role::Pmm)) {
            return Status::BadState;
        }
        auto const now = m_pins.load(std::memory_order_relaxed);
        if (now == std::numeric_limits<u8>::max()) {
            return Status::NoResource;
        }
        m_pins.store(static_cast<u8>(now + 1), std::memory_order_relaxed);
        return Status::Ok;
    }

    /// One pin fewer. BadState, and nothing changed, when the frame holds
    /// none: the count is unsigned, so an unpin of none would wrap to the
    /// limit and hold the frame for ever. The mirror of pin(), and under the
    /// same lock.
    [[nodiscard]]
    auto unpin() -> Status {
        auto const now = m_pins.load(std::memory_order_relaxed);
        if (now == 0) {
            return Status::BadState;
        }
        m_pins.store(static_cast<u8>(now - 1), std::memory_order_relaxed);
        return Status::Ok;
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
    /// the buddy writes the role alone through enter_free_list() and
    /// leave_free_list(), and the boot stamp through stamp_role(). Reading a frame -- taking its
    /// address, dumping it, asking what it is -- must never change it.
    template <typename Body>
    FORCE_INLINE
    auto become() -> Body & {
        static_assert(sizeof(Body) <= sizeof(m_body), "role payload is too large");

        for (auto &byte: m_body) {
            byte = std::byte{};
        }
        set_status<Index::Role>(Body::ROLE);
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
    /// A tail's distance from its head: written only by the head's own
    /// block operations (make_block(), split(), clear_block()), so that a
    /// tail cannot be told it belongs to a head that does not know it.
    FORCE_INLINE
    auto set_compound_offset(u32 offset) -> void {
        DEBUG_ASSERT(offset <= std::numeric_limits<u16>::max(),
                     "frame {} of a block: no block is that large", offset);
        m_compound.store(static_cast<u16>(offset), std::memory_order_relaxed);
    }

    /// Stamps the |count| frames from |head| on as one block: the head's
    /// offset 0, each tail its distance from it.
    auto stamp_tails(usize const head, usize const count) -> void {
        for (usize i = 0; i < count; ++i) {
            this[head + i].set_compound_offset(static_cast<u32>(i));
        }
    }

    /// The status word as it is now, decoded from one relaxed load.
    ///
    /// The word is read by more than the frame's owner: the buddy reads a
    /// partner it does not own to decide a merge, and anyone may ask a frame
    /// its node, zone or section. A plain word read while its owner writes it
    /// is a data race, undefined however harmless the hardware makes it, so
    /// the word is an atomic. Relaxed is enough: what decides a merge, the Pmm
    /// role and its order, changes only under the lock of the zone the frame
    /// belongs to, which orders it for that zone's buddy; a partner of another
    /// zone is turned away by its zone and node, which never change after
    /// init; and every other field is true of the frame whenever it is read.
    FORCE_INLINE
    auto status() const -> page::StatusWord {
        return page::StatusWord::from_raw(m_status.load(std::memory_order_relaxed));
    }

    /// Rewrites field |Id| of the word: a relaxed load and a relaxed store,
    /// not a read-modify-write. The word has one writer at a time -- the
    /// frame's owner, or the buddy under the zone lock while the frame is
    /// free -- so nothing writes between the two, and a reader sees the word
    /// before or after, never torn.
    template <auto Id>
    FORCE_INLINE
    auto set_status(page::StatusWord::ValueTypeOf<Id> const &value) -> void {
        auto word = status();
        word.template set<Id>(value);
        m_status.store(word.template into<page::StatusWord::StorageUnit>(),
                       std::memory_order_relaxed);
    }

    std::atomic<page::StatusWord::StorageUnit> m_status;

    // The size of following fields type should be half of a word.
    mutable std::atomic<u32> m_references;

    // Trace the position of current frame in a large frame. Sixteen bits:
    // the largest block has 2^MAX_FRAME_ORDER frames.
    mutable std::atomic<u16> m_compound;

    // See pin_count(). It shares the half word the compound offset had, and
    // leaves one byte of it spare.
    std::atomic<u8> m_pins;

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

static_assert((usize{1} << MAX_FRAME_ORDER) <= usize{std::numeric_limits<u16>::max()} + 1,
              "a frame's offset in its block is stored in sixteen bits");
// The status word is read and written with plain loads and stores of one
// word; a status atomic that needed a lock would hide one in every
// accessor.
static_assert(std::atomic<page::StatusWord::StorageUnit>::is_always_lock_free,
              "the frame's status word is a lock-free atomic");
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
