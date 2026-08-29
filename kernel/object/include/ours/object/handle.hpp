/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_HPP
#define OURS_OBJECT_HANDLE_HPP 1

#include <ours/object/types.hpp>
#include <ours/object/dispatcher.hpp>

#include <ustl/rc.hpp>
#include <ustl/box.hpp>
#include <ustl/sync/atomic.hpp>

#include <concepts>


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
            DEBUG_ASSERT(object); 
        }

        template <typename U>
        FORCE_INLINE
        KernelHandle(KernelHandle<U> &&handle)
            : m_dispatcher(ustl::move(handle).m_dispatcher)
        {}

        ~KernelHandle() = default;

        FORCE_INLINE
        auto dispatcher() const -> ustl::Arc<T> const & {
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