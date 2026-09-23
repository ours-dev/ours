/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/object/handle.hpp>
#include <ours/sync/mutex.hpp>

#include <atomic>
#include <new>

#include <cstddef>

#include <gktl/counter.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(OBJECT_HANDLE_ARENA_EXHAUSTED, "object.handle.arena_exhausted");

namespace ours::object {
namespace {
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
    ///  - the base value of the Handle living in it right now, or zero. A
    ///    lookup compares against this without taking the arena lock, which
    ///    is why it is atomic.
    ///
    /// Zircon counterpart: HandleTableArena, over fbl::GPArena. GPArena keeps
    /// the base value inside the freed slot (PreserveSize) and reads it back;
    /// keeping it outside is the same information without reading an object
    /// after its destructor ran.
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

            // The generation counts up from whatever this slot last carried,
            // as Zircon's NewHandleValue() does -- with one difference: it
            // skips zero when it wraps. Generation zero is reserved to mean
            // "no handle": a free slot records a live base value of 0, and
            // index 0 at generation 0 is also 0, so a handle there would be
            // indistinguishable from an empty slot (and value 0b11 would find
            // it). lookup() rejects generation zero for the same reason.
            auto const old_generation =
                (m_last_base_value[index] & HANDLE_GENERATION_MASK) >> HANDLE_GENERATION_SHIFT;
            auto generation =
                ((old_generation + 1) << HANDLE_GENERATION_SHIFT) & HANDLE_GENERATION_MASK;
            if (generation == 0) {
                generation = u32{1} << HANDLE_GENERATION_SHIFT;
            }
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
            m_live_base_value[base_value & HANDLE_INDEX_MASK].store(base_value,
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

        [[nodiscard]]
        auto lookup(u32 const base_value) -> Handle * {
            // Generation zero never names a handle (see reserve()), and
            // letting it through would compare it against the 0 that marks
            // an empty slot -- and hand back storage nothing lives in.
            if ((base_value & HANDLE_GENERATION_MASK) == 0) {
                return nullptr;
            }
            auto const index = base_value & HANDLE_INDEX_MASK;
            if (index >= HANDLE_ARENA_SLOTS) {
                return nullptr;
            }
            // TODO(P1): a constant-time comparison, as Zircon's
            // conditional_select_nospec_eq(). A branch on this equality lets a
            // program probe which slots are live by timing.
            if (m_live_base_value[index].load(std::memory_order_acquire) != base_value) {
                return nullptr;
            }
            return std::launder(reinterpret_cast<Handle *>(&m_storage[index]));
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

    HandleArena handle_arena;
} // namespace

Handle::Handle(ustl::Arc<Dispatcher> dispatcher, Rights const rights, u32 const base_value)
    : m_table_koid(0)
    , m_dispatcher(ustl::move(dispatcher))
    , m_rights(rights)
    , m_base_value(base_value) {}

Handle::~Handle() {
    handle_arena.unpublish(m_base_value);

    // Zircon Handle::Delete(): the handle count drops with the handle, and the
    // last one out tells the object. That notification can do real work --
    // a channel end raises PEER_CLOSED on its peer -- which is why no Handle
    // is ever destroyed while a table lock is held (see HandleTable).
    if (m_dispatcher && m_dispatcher->decrease_handle_count()) {
        m_dispatcher->on_zero_handles();
    }
}

auto Handle::Disposer::operator()(Handle *const handle) const noexcept -> void {
    // The destructor has run and the object is gone, so the slot is found
    // from the address alone -- nothing here reads the destroyed Handle. The
    // generation it carried is already in the arena's own records.
    handle_arena.release(handle_arena.index_of(handle));
}

auto Handle::reserve_slot() -> ustl::Option<u32> {
    auto base_value = handle_arena.reserve();
    if (!base_value) {
        // Not a panic: a program that opens handles without closing them can
        // reach this, and what a program can reach must not take the kernel
        // down. Zircon logs throttled and returns null here too.
        OBJECT_HANDLE_ARENA_EXHAUSTED.add(1);
        log::warn("object: the handle arena is full ({} slots)", HANDLE_ARENA_SLOTS);
    }
    return base_value;
}

auto Handle::emplace(u32 const base_value, ustl::Arc<Dispatcher> dispatcher, Rights const rights)
    -> ustl::Box<Self> {
    dispatcher->increase_handle_count();
    // Placement new rather than std::construct_at: the constructor is
    // private, so only a member of Handle may name it.
    auto *const handle =
        ::new (handle_arena.slot(base_value)) Self(ustl::move(dispatcher), rights, base_value);
    handle_arena.publish(base_value);
    return ustl::Box<Self>::from_raw(handle);
}

auto Handle::make(KernelHandle<Dispatcher> kernel_handle, Rights const rights) -> ustl::Box<Self> {
    if (!kernel_handle.dispatcher()) {
        return ustl::Box<Self>();
    }

    // The slot first, and only then the reference, in Zircon's order. If the
    // arena is full the KernelHandle still holds the object when this
    // returns, so its destructor tells the object it lost its last holder --
    // a channel end dropped here raises PEER_CLOSED on its peer. Taking the
    // reference first, as this used to, let the object go quietly.
    auto const base_value = reserve_slot();
    if (!base_value) {
        return ustl::Box<Self>();
    }

    // release(), not a copy: the reference moves from the KernelHandle into
    // the Handle, and the object must not be told it lost a holder -- it is
    // gaining one.
    return emplace(*base_value, kernel_handle.release(), rights);
}

auto Handle::make(ustl::Arc<Dispatcher> kernel_handle, Rights const rights) -> ustl::Box<Self> {
    if (!kernel_handle) {
        return ustl::Box<Self>();
    }
    auto const base_value = reserve_slot();
    if (!base_value) {
        return ustl::Box<Self>();
    }
    return emplace(*base_value, ustl::move(kernel_handle), rights);
}

auto Handle::clone(Self *const kernel_handle, Rights const rights) -> ustl::Box<Self> {
    DEBUG_ASSERT(kernel_handle);
    DEBUG_ASSERT(kernel_handle->has_rights(rights), "a clone may not widen its source's rights");
    auto const base_value = reserve_slot();
    if (!base_value) {
        return ustl::Box<Self>();
    }
    return emplace(*base_value, kernel_handle->m_dispatcher, rights);
}

auto Handle::from_base_value(u32 const base_value) -> Self * {
    return handle_arena.lookup(base_value);
}

} // namespace ours::object
