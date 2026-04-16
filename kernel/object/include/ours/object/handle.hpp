/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_HPP
#define OURS_OBJECT_HANDLE_HPP 1

#include <ours/object/types.hpp>
#include <ours/object/dispatcher.hpp>

#include <ustl/rc.hpp>
#include <ustl/mem/box.hpp>
#include <ustl/sync/atomic.hpp>

#include <ustl/collections/deque.hpp>

namespace ours::object {
    template <typename KernelObject>
    class KernelHandle;

    class Handle final {
        using Self = Handle;
      public:
        static auto make(KernelHandle<Dispatcher> kernel_handle, Rights rights) -> ustl::mem::Box<Self>;

        static auto make(ustl::Rc<Dispatcher> kernel_handle, Rights rights) -> ustl::mem::Box<Self>;

        static auto clone(Self *kernel_handle, Rights rights) -> ustl::mem::Box<Self>;
      private:
        ustl::sync::Atomic<KoId> m_table_koid;
        ustl::Rc<Dispatcher> m_dispatcher;

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
        KernelHandle(ustl::Rc<U> object)
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
        auto dispatcher() const -> ustl::Rc<T> const & {
            return m_dispatcher;
        }

        template <typename U>
        FORCE_INLINE
        auto reset(ustl::Rc<U> dispatcher) -> void {
            static_assert(ustl::traits::IsBaseOfV<Dispatcher, U>);
            if (m_dispatcher) {
                m_dispatcher->on_zero();
            }

            m_dispatcher = ustl::move(dispatcher);
        }

        USTL_NOCOPYABLE(KernelHandle);
      private:
        ustl::Rc<T> m_dispatcher;
    };

    template <typename KernelObject>
    KernelHandle(KernelObject *) -> KernelHandle<KernelObject>;

    template <typename KernelObject>
    KernelHandle(ustl::Rc<KernelObject>) -> KernelHandle<KernelObject>;

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_HANDLE_HPP