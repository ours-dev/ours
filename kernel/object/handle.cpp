/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/aspace_layout.hpp>
#include <ours/handle.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall.hpp>

#include <atomic>
#include <new>

#include <cstddef>

#include <arch/nospec.hpp>
#include <gktl/counter.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(OBJECT_HANDLE_ARENA_EXHAUSTED, "object.handle.arena_exhausted");

namespace ours::object {
namespace {
    /// Set in the live record of an occupied slot, and never in a base value.
    ///
    /// A base value is 30 bits (HANDLE_RESERVED_BITS are shifted out of the
    /// user's number), so bit 31 is free, and marking live records with it
    /// is what lets lookup() be a single comparison: an empty slot records 0,
    /// which no marked probe can equal, whatever generation or index the
    /// probe names. Before the mark, generation 0 had to be reserved to mean
    /// "empty" and lookup() had to reject it with a branch of its own.
    constexpr u32 HANDLE_LIVE_MARK = u32{1} << 31;
    static_assert(HANDLE_LIVE_MARK > (HANDLE_GENERATION_MASK | HANDLE_INDEX_MASK),
                  "the live mark must lie outside every base value");

    /// The system-wide pool every Handle lives in.
    ///
    /// Equal-sized slots at consecutive addresses, so that a slot index is a
    /// name for a Handle; that is the whole reason handles do not simply come
    /// from kmalloc (handle-lookup.md). Everything about a slot that has to
    /// outlive the Handle in it is kept beside the storage rather than inside
    /// it, so nothing ever reads a destroyed object:
    ///
    ///  - the base value it last carried, from which the next generation is
    ///    counted -- that is what makes a value kept past close() stop
    ///    resolving once the slot is reused;
    ///  - the base value of the Handle living in it right now, marked with
    ///    HANDLE_LIVE_MARK, or zero. A lookup compares against this without
    ///    taking the arena lock, which is why it is atomic.
    class HandleArena final {
      public:
        HandleArena() : m_lock(), m_never_used(0), m_free_count(0) {}

        /// Claims a slot and gives it its next base value; none when every
        /// slot is taken.
        [[nodiscard]]
        auto reserve() -> ustl::Option<u32> {
            Guard guard{m_lock};

            u32 index = 0;
            if (m_free_count > 0) {
                m_free_count -= 1;
                index = m_free[m_free_count];
            } else if (m_never_used < HANDLE_ARENA_SLOTS) {
                index = m_never_used;
                m_never_used += 1;
            } else {
                return ustl::none();
            }

            // The generation counts up from whatever this slot last carried and
            // wraps through zero. Zero used to be skipped, because an empty
            // slot recorded 0 and index 0 at generation 0 is also 0; the live
            // mark keeps the two apart now (see HANDLE_LIVE_MARK).
            auto const old_generation =
                (m_last_base_value[index] & HANDLE_GENERATION_MASK) >> HANDLE_GENERATION_SHIFT;
            auto const generation =
                ((old_generation + 1) << HANDLE_GENERATION_SHIFT) & HANDLE_GENERATION_MASK;
            auto const base_value = index | generation;
            m_last_base_value[index] = base_value;
            return ustl::some(base_value);
        }

        /// The storage a reserved |base_value| is to be constructed in.
        [[nodiscard]]
        auto slot(u32 const base_value) -> void * {
            return &m_storage[base_value & HANDLE_INDEX_MASK];
        }

        /// Makes a Handle constructed in its slot visible to lookups.
        auto publish(u32 const base_value) -> void {
            m_live_base_value[base_value & HANDLE_INDEX_MASK].store(base_value | HANDLE_LIVE_MARK,
                                                                    std::memory_order_release);
        }

        /// Hides a Handle from lookups. Called first thing in its destructor,
        /// so that nothing can find the object while it is being torn down.
        auto unpublish(u32 const base_value) -> void {
            m_live_base_value[base_value & HANDLE_INDEX_MASK].store(0, std::memory_order_release);
        }

        /// The slot index of a Handle living in this arena.
        [[nodiscard]]
        auto index_of(Handle const *const handle) const -> u32 {
            auto const offset = reinterpret_cast<std::byte const *>(handle) -
                                reinterpret_cast<std::byte const *>(&m_storage[0]);
            return static_cast<u32>(static_cast<usize>(offset) / sizeof(Slot));
        }

        /// Returns the slot at |index|, whose Handle has been destroyed.
        auto release(u32 const index) -> void {
            Guard guard{m_lock};
            m_free[m_free_count] = static_cast<u16>(index);
            m_free_count += 1;
        }

        /// The live Handle whose base value is |base_value|, or null.
        ///
        /// |base_value| comes straight from a number the user passed, so this
        /// runs on every system call that names a handle, and every step is
        /// written to hold under speculative execution (Spectre V1) as well as
        /// on the architectural path. A branch on "is the index in range" or
        /// "does the slot hold this value" would be predicted, and a
        /// mispredicted path would go on to use a Handle the user chose --
        /// its dispatcher, its rights -- before the check resolved, leaving
        /// that choice readable in the cache. So there is no branch:
        ///
        ///  - the index is confined, not checked. An index at or past the
        ///    arena's end becomes 0, and the comparison below then fails by
        ///    itself, because the probe still names the original index and no
        ///    live record at slot 0 can equal it;
        ///  - the comparison selects, not branches: the result is the slot's
        ///    address or 0, by a conditional move that cannot execute before
        ///    the comparison has.
        ///
        /// The 0 a failed lookup yields is safe to touch speculatively as
        /// well. A caller's `if (!handle)` is itself a branch, and past a
        /// misprediction it may load through the null pointer at a small
        /// offset; nothing is mapped below USER_ASPACE_BASE in any address
        /// space, so that load reads no data and fills no cache line.
        ///
        /// The TODO this replaces called the concern timing, a program
        /// probing which slots are live. That is not what the select is for:
        /// liveness is not a secret worth a mitigation. The select is for
        /// Spectre V1.
        [[nodiscard]]
        auto lookup(u32 const base_value) -> Handle * {
            static_assert(USER_ASPACE_BASE == 2_mb,
                          "a failed lookup's null result is touched speculatively; see above");
            auto const index =
                arch::confine_array_index(base_value & HANDLE_INDEX_MASK, HANDLE_ARENA_SLOTS);
            auto const live = m_live_base_value[index].load(std::memory_order_acquire);
            auto const address = arch::conditional_select_nospec_eq(
                live, base_value | HANDLE_LIVE_MARK, reinterpret_cast<usize>(&m_storage[index]), 0);
            auto *const handle = reinterpret_cast<Handle *>(address);
            // std::launder only for a real address: the Handle there was made
            // by placement new into byte storage. The test is on the selected
            // result, which is the slot's address only if the comparison
            // really matched -- a cmov's consumers wait for its comparison.
            return handle ? std::launder(handle) : nullptr;
        }

      private:
        using Slot = std::byte[sizeof(Handle)];

        sync::Mutex m_lock;

        /// Slots at and above this index have never been handed out.
        u32 m_never_used TA_GUARDED(m_lock);

        /// A stack of released slot indices.
        u32 m_free_count TA_GUARDED(m_lock);

        // The arrays below have no initialisers on purpose. The arena is an
        // object of static storage duration, so they start zeroed -- which is
        // exactly "never carried a value" and "nothing live" -- and an
        // initialiser would only have the constructor clear a quarter of a
        // megabyte at boot that is already clear.
        u16 m_free[HANDLE_ARENA_SLOTS] TA_GUARDED(m_lock);
        u32 m_last_base_value[HANDLE_ARENA_SLOTS] TA_GUARDED(m_lock);
        std::atomic<u32> m_live_base_value[HANDLE_ARENA_SLOTS];
        alignas(Handle) Slot m_storage[HANDLE_ARENA_SLOTS];
    };
    static_assert(HANDLE_ARENA_SLOTS <= 65536, "free slots are recorded as u16");

    // The arena is sized statically, HANDLE_ARENA_SLOTS times this, so a
    // member added to Handle costs every slot at once. Pinned here so that the
    // cost is a decision rather than a side effect. Widening KoId to 64 bits
    // did not move it: m_table_koid took the four bytes of padding that sat
    // before m_dispatcher's pointer alignment. m_account did, from 40 to 48:
    // 32 KiB across today's 4096 slots, the price of charging a handle to the
    // process that made it wherever the handle goes (HANDLE_MAX_PER_PROCESS).
    static_assert(sizeof(Handle) == 48, "Handle's size is the arena's size; see above");

    /// The account a handle made now is charged to: the calling process's,
    /// or none when the kernel makes it on its own behalf (the root job handle
    /// the first program starts with). Empty with |refused| set when the
    /// process is at HANDLE_MAX_PER_PROCESS.
    auto charge_current(bool *const refused) -> ustl::Arc<HandleAccount> {
        *refused = false;
        auto *const process = ProcessDispatcher::current();
        if (process == nullptr) {
            return {};
        }
        auto account = process->handle_account();
        if (!account->try_charge()) {
            *refused = true;
            return {};
        }
        return account;
    }

    HandleArena HANDLE_ARENA;
} // namespace

auto HandleAccount::try_charge() -> bool {
    auto live = m_live.load(std::memory_order_relaxed);
    do {
        if (live >= HANDLE_MAX_PER_PROCESS) {
            return false;
        }
    } while (!m_live.compare_exchange_weak(live, live + 1, std::memory_order_relaxed));
    return true;
}

auto HandleAccount::refund() -> void {
    DEBUG_ASSERT(m_live.load(std::memory_order_relaxed) != 0, "a refund with nothing charged");
    m_live.fetch_sub(1, std::memory_order_relaxed);
}

Handle::Handle(ustl::Arc<Dispatcher> dispatcher, Rights const rights, u32 const base_value,
               ustl::Arc<HandleAccount> account)
    : m_table_koid(0)
    , m_dispatcher(ustl::move(dispatcher))
    , m_account(ustl::move(account))
    , m_rights(rights)
    , m_base_value(base_value) {}

Handle::~Handle() {
    HANDLE_ARENA.unpublish(m_base_value);

    // Every wait made through this handle ends with it. A handle normally
    // leaves its table first, which has already cancelled them
    // (HandleTable::unlink_locked(), clean()), and then this finds nothing.
    // It is here for any path that destroys a handle without going through a
    // table. A wait that outlived its handle would be worse than a wait that
    // never ends: a PortObserver left linked on an object nothing refers to
    // would never be freed and would keep its port alive, and it names the
    // object by a bare pointer, which outlives the object.
    if (m_dispatcher) {
        m_dispatcher->cancel(this);
    }

    // The handle count drops with the handle, and the last one out tells the
    // object. That notification can do real work -- a channel end raises
    // PEER_CLOSED on its peer -- which is why no Handle is ever destroyed while
    // a table lock is held (see HandleTable).
    if (m_dispatcher && m_dispatcher->decrease_handle_count()) {
        m_dispatcher->on_zero_handles();
    }

    // The creator's room back, whether the handle died in its table, in
    // someone else's, or in a message nobody read.
    if (m_account) {
        m_account->refund();
    }
}

auto Handle::Disposer::operator()(Handle *const handle) const noexcept -> void {
    // The destructor has run and the object is gone, so the slot is found
    // from the address alone -- nothing here reads the destroyed Handle. The
    // generation it carried is already in the arena's own records.
    HANDLE_ARENA.release(HANDLE_ARENA.index_of(handle));
}

auto Handle::reserve_slot() -> ustl::Option<u32> {
    auto base_value = HANDLE_ARENA.reserve();
    if (!base_value) {
        // Not a panic: a program that opens handles without closing them can
        // reach this, and what a program can reach must not take the kernel
        // down.
        OBJECT_HANDLE_ARENA_EXHAUSTED.add(1);
        log::warn("object: the handle arena is full ({} slots)", HANDLE_ARENA_SLOTS);
    }
    return base_value;
}

auto Handle::emplace(u32 const base_value, ustl::Arc<Dispatcher> dispatcher, Rights const rights,
                     ustl::Arc<HandleAccount> account) -> ustl::Box<Self> {
    dispatcher->increase_handle_count();
    // Placement new rather than std::construct_at: the constructor is
    // private, so only a member of Handle may name it.
    auto *const handle = ::new (HANDLE_ARENA.slot(base_value))
        Self(ustl::move(dispatcher), rights, base_value, ustl::move(account));
    HANDLE_ARENA.publish(base_value);
    return ustl::Box<Self>::from_raw(handle);
}

auto Handle::make(KernelHandle<Dispatcher> kernel_handle, Rights const rights) -> ustl::Box<Self> {
    if (!kernel_handle.dispatcher()) {
        return ustl::Box<Self>();
    }

    // The account and the slot first, and only then the reference. If either
    // is refused the KernelHandle still holds the object when this returns, so
    // its destructor tells the object it lost its last holder -- a channel end
    // dropped here raises PEER_CLOSED on its peer. Taking the reference first,
    // as this used to, let the object go quietly.
    auto refused = false;
    auto account = charge_current(&refused);
    if (refused) {
        return ustl::Box<Self>();
    }
    auto const base_value = reserve_slot();
    if (!base_value) {
        if (account) {
            account->refund();
        }
        return ustl::Box<Self>();
    }

    // release(), not a copy: the reference moves from the KernelHandle into
    // the Handle, and the object must not be told it lost a holder -- it is
    // gaining one.
    return emplace(*base_value, kernel_handle.release(), rights, ustl::move(account));
}

auto Handle::make(ustl::Arc<Dispatcher> kernel_handle, Rights const rights) -> ustl::Box<Self> {
    if (!kernel_handle) {
        return ustl::Box<Self>();
    }
    auto refused = false;
    auto account = charge_current(&refused);
    if (refused) {
        return ustl::Box<Self>();
    }
    auto const base_value = reserve_slot();
    if (!base_value) {
        if (account) {
            account->refund();
        }
        return ustl::Box<Self>();
    }
    return emplace(*base_value, ustl::move(kernel_handle), rights, ustl::move(account));
}

auto Handle::make_uncharged(ustl::Arc<Dispatcher> dispatcher, Rights const rights)
    -> ustl::Box<Self> {
    if (!dispatcher) {
        return ustl::Box<Self>();
    }
    auto const base_value = reserve_slot();
    if (!base_value) {
        return ustl::Box<Self>();
    }
    return emplace(*base_value, ustl::move(dispatcher), rights, ustl::Arc<HandleAccount>());
}

auto Handle::clone(Self *const source, Rights const rights) -> ustl::Box<Self> {
    DEBUG_ASSERT(source);
    DEBUG_ASSERT(source->has_rights(rights), "a clone may not widen its source's rights");
    auto refused = false;
    auto account = charge_current(&refused);
    if (refused) {
        return ustl::Box<Self>();
    }
    auto const base_value = reserve_slot();
    if (!base_value) {
        if (account) {
            account->refund();
        }
        return ustl::Box<Self>();
    }
    return emplace(*base_value, source->m_dispatcher, rights, ustl::move(account));
}

auto Handle::from_base_value(u32 const base_value) -> Self * {
    return HANDLE_ARENA.lookup(base_value);
}

} // namespace ours::object
