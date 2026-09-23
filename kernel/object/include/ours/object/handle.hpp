/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_HPP
#define OURS_OBJECT_HANDLE_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/types.hpp>

#include <atomic>
#include <concepts>

#include <ustl/box.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/rc.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
template <typename KernelObject>
class KernelHandle;

/// How a handle's base value is laid out (docs/architecture/object/handle-lookup.md).
///
///   [31..30]  reserved, always 0 -- so that the value can be shifted left by
///             two to make room for the fixed low bits of a user value
///   [29..18]  generation, bumped every time the arena slot is reused
///   [17..0]   arena index
///
/// The index space is 2^18 slots, as in Zircon; only the first
/// HANDLE_ARENA_SLOTS of them are backed by memory in this first version.
constexpr u32 HANDLE_RESERVED_BITS = 2;
constexpr u32 HANDLE_INDEX_BITS = 18;
constexpr u32 HANDLE_MAX_COUNT = u32{1} << HANDLE_INDEX_BITS;
constexpr u32 HANDLE_INDEX_MASK = HANDLE_MAX_COUNT - 1;
constexpr u32 HANDLE_GENERATION_SHIFT = HANDLE_INDEX_BITS;
constexpr u32 HANDLE_GENERATION_MASK =
    ((u32{1} << (32 - HANDLE_RESERVED_BITS)) - 1) & ~HANDLE_INDEX_MASK;
static_assert((HANDLE_MAX_COUNT & (HANDLE_MAX_COUNT - 1)) == 0,
              "the index is taken with a mask, so the count must be a power of two");
static_assert((HANDLE_GENERATION_MASK >> HANDLE_GENERATION_SHIFT) >= 255,
              "a generation that wraps within a few reuses defends nothing");

/// How many arena slots exist. Fixed and backed up front: the kernel address
/// space cannot fault pages in on demand, so Zircon's "reserve 2^18, commit
/// as used" has to wait for an explicit commit path (P1). 4096 handles across
/// the whole system is ample for a handful of boot programs, and running out
/// is an ordinary NoResource, not a panic.
constexpr u32 HANDLE_ARENA_SLOTS = u32{1} << 12;
static_assert(HANDLE_ARENA_SLOTS <= HANDLE_MAX_COUNT);

/// One entry in a handle table: an object, the rights this entry grants over
/// it, and which table it belongs to.
///
/// Rights live here, not on the object -- two handles to one channel may
/// grant different things, and a system call checks the handle it was given.
///
/// Handles live in a single system-wide arena of equal-sized slots, and that
/// is what makes a handle value possible: a slot's address and its index are
/// one arithmetic step apart, so a small number can name a Handle with no
/// per-process map in between. A Handle is therefore only ever created by
/// make()/clone() and only ever owned through a ustl::Box, whose Disposer
/// returns the slot to the arena.
///
/// Zircon counterpart: Handle and HandleOwner (kernel/object/handle.cc).
class Handle final {
    using Self = Handle;

  public:
    /// Returns a destroyed Handle's slot to the arena. Box runs ~Handle()
    /// first; this only reclaims the storage, per ustl's Disposer contract.
    struct Disposer {
        auto operator()(Handle *handle) const noexcept -> void;
    };

    /// A new handle to the object |kernel_handle| held, taking over its
    /// reference. Empty when the arena is exhausted.
    static auto make(KernelHandle<Dispatcher> kernel_handle, Rights rights) -> ustl::Box<Self>;

    /// A new handle to |dispatcher|, as one more holder of it.
    static auto make(ustl::Arc<Dispatcher> kernel_handle, Rights rights) -> ustl::Box<Self>;

    /// A second handle to the same object, with |rights| -- which the caller
    /// has already checked are no more than |kernel_handle| grants.
    static auto clone(Self *kernel_handle, Rights rights) -> ustl::Box<Self>;

    /// The live handle whose base value is |base_value|, if there is one.
    ///
    /// Two of the five checks a handle value passes (handle-lookup.md): the
    /// index must be inside the backed part of the arena, and the slot must
    /// currently hold a handle with exactly this base value -- which a value
    /// kept past its handle's close does not, because the slot's generation
    /// has moved on. Which table the handle belongs to is the caller's check.
    [[nodiscard]]
    static auto from_base_value(u32 base_value) -> Self *;

    /// Drops this holder of the object: the last handle to go tells the
    /// object it has no holders left (on_zero_handles()).
    ~Handle();

    [[nodiscard]]
    auto dispatcher() const -> ustl::Arc<Dispatcher> const & {
        return m_dispatcher;
    }

    [[nodiscard]]
    auto rights() const -> Rights {
        return m_rights;
    }

    /// Whether this handle grants every right in |desired|.
    [[nodiscard]]
    auto has_rights(Rights const desired) const -> bool {
        return (m_rights & desired) == desired;
    }

    [[nodiscard]]
    auto base_value() const -> u32 {
        return m_base_value;
    }

    /// The koid of the table this handle is in, or zero while it is in none.
    [[nodiscard]]
    auto table_koid() const -> KoId {
        return m_table_koid.load(std::memory_order_relaxed);
    }

    auto set_table_koid(KoId const koid) -> void {
        m_table_koid.store(koid, std::memory_order_relaxed);
    }

    USTL_NOCOPYABLE(Handle);

  private:
    Handle(ustl::Arc<Dispatcher> dispatcher, Rights rights, u32 base_value);

    /// Where a new handle for |dispatcher| goes. The dispatcher's handle
    /// count is raised here, before the Handle exists, so that it can never
    /// be observed lower than the number of handles that name it.
    static auto emplace(ustl::Arc<Dispatcher> dispatcher, Rights rights) -> ustl::Box<Self>;

    std::atomic<KoId> m_table_koid;
    ustl::Arc<Dispatcher> m_dispatcher;
    Rights const m_rights;
    u32 const m_base_value;

    /// Links this handle into the one HandleTable it is in. Guarded by that
    /// table's lock.
    ustl::collections::intrusive::ListMemberHook<> m_table_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_table_hook, TableOption);

  public:
    USTL_DECLARE_LIST(Self, TableList, TableOption);
};

template <typename T>
class KernelHandle {
    using Self = KernelHandle;

  public:
    KernelHandle() = default;

    FORCE_INLINE
    KernelHandle(T *object)
        : m_dispatcher(object) {
        DEBUG_ASSERT(object);
    }

    template <typename U>
    FORCE_INLINE
    KernelHandle(ustl::Arc<U> object)
        : m_dispatcher(ustl::move(object)) {
        // The member initialiser above already moved out of `object`, and
        // ustl::Rc's move constructor nulls the source (std::exchange on
        // m_pointer), so asserting on the parameter here asserted on a
        // guaranteed-null pointer: every well-formed construction of this
        // overload fired it. It never showed because nothing had constructed a
        // KernelHandle from an Arc yet. The state worth checking is the one
        // that survives the constructor.
        DEBUG_ASSERT(m_dispatcher);
    }

    template <typename U>
    FORCE_INLINE
    KernelHandle(KernelHandle<U> &&handle)
        : m_dispatcher(ustl::move(handle).m_dispatcher) {}

    /// Move-assignment, and no copy-assignment.
    ///
    /// USTL_NOCOPYABLE below deletes the copy operations, and deleting the
    /// copy constructor suppresses the implicit move ones -- so without this
    /// there was no way to *store* a KernelHandle into one that already
    /// exists, only to construct a fresh one. Every factory in this layer
    /// reports its object through a `KernelHandle<T> &` out-parameter the
    /// caller declared (ProcessDispatcher::create takes two), which is
    /// precisely that assignment, so the type could not be used the way its
    /// own interfaces ask for it. Zircon's KernelHandle declares the same
    /// operator for the same reason.
    FORCE_INLINE
    auto operator=(KernelHandle &&other) -> KernelHandle & {
        reset(other.release());
        return *this;
    }

    /// A KernelHandle counts as a handle: dropping the last one is the object
    /// losing its last holder, exactly as closing its last user handle is.
    ///
    /// It used to be defaulted, which meant an object that never reached a
    /// user handle table -- one end of a channel pair whose other end was
    /// handed out and whose own end was dropped on an error path, say -- never
    /// heard that it had lost its holders, and a peer waiting on PEER_CLOSED
    /// would wait forever. Zircon's KernelHandle destructor does the same.
    /// Moving a KernelHandle into a user Handle goes through release(), which
    /// hands the reference over without this notification.
    ~KernelHandle() {
        if (m_dispatcher) {
            m_dispatcher->on_zero_handles();
        }
    }

    /// Gives up the object without telling it it lost a holder: the caller
    /// is taking the reference somewhere that will count it (Handle::make()).
    [[nodiscard]]
    FORCE_INLINE
    auto release() -> ustl::Arc<T> {
        return ustl::move(m_dispatcher);
    }

    /// Two overloads, because ustl::Rc propagates constness through
    /// `operator->` (`Rc<T> const` yields `T const *`), unlike fbl::RefPtr and
    /// std::shared_ptr, whose constness is shallow. With only the const one, a
    /// non-const KernelHandle still yielded a `T const *` and every dispatcher
    /// method -- all of which are non-const, since they mutate the object they
    /// stand for -- was unreachable through the handle that owns it.
    FORCE_INLINE
    auto dispatcher() const -> ustl::Arc<T> const & {
        return m_dispatcher;
    }

    FORCE_INLINE
    auto dispatcher() -> ustl::Arc<T> & {
        return m_dispatcher;
    }

    /// Replaces the object held, telling the old one it lost a holder.
    ///
    /// It called `on_zero()`, which does not exist on any dispatcher; it
    /// compiled only because nothing had instantiated it.
    template <std::derived_from<Dispatcher> U>
    FORCE_INLINE
    auto reset(ustl::Arc<U> dispatcher) -> void {
        if (m_dispatcher) {
            m_dispatcher->on_zero_handles();
        }

        m_dispatcher = ustl::move(dispatcher);
    }

    FORCE_INLINE
    auto reset() -> void {
        reset(ustl::Arc<T>());
    }

    USTL_NOCOPYABLE(KernelHandle);

  private:
    ustl::Arc<T> m_dispatcher;
};

template <typename KernelObject>
KernelHandle(KernelObject *) -> KernelHandle<KernelObject>;

template <typename KernelObject>
KernelHandle(ustl::Arc<KernelObject>) -> KernelHandle<KernelObject>;

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_HANDLE_HPP