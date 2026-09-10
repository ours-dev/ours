/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_HPP
#define OURS_OBJECT_HANDLE_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/types.hpp>

#include <concepts>

#include <ustl/box.hpp>
#include <ustl/rc.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours::object {
template <typename KernelObject>
class KernelHandle;

class Handle final {
    using Self = Handle;

  public:
    static auto make(KernelHandle<Dispatcher> kernel_handle, Rights rights) -> ustl::Box<Self>;

    static auto make(ustl::Arc<Dispatcher> kernel_handle, Rights rights) -> ustl::Box<Self>;

    static auto clone(Self *kernel_handle, Rights rights) -> ustl::Box<Self>;

  private:
    ustl::sync::Atomic<KoId> m_table_koid;
    ustl::Arc<Dispatcher> m_dispatcher;

    Rights m_rights;
    u32 m_baseval;
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
        m_dispatcher = ustl::move(other.m_dispatcher);
        return *this;
    }

    ~KernelHandle() = default;

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

    template <std::derived_from<Dispatcher> U>
    FORCE_INLINE
    auto reset(ustl::Arc<U> dispatcher) -> void {
        if (m_dispatcher) {
            m_dispatcher->on_zero();
        }

        m_dispatcher = ustl::move(dispatcher);
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