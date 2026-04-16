/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_DISPATCHER_HPP
#define OURS_OBJECT_DISPATCHER_HPP 1

#include <ours/types.hpp>
#include <ours/rights.hpp>
#include <ours/compiler.hpp>
#include <ours/signals.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/object/types.hpp>
#include <ours/object/observer.hpp>

#include <ustl/rc.hpp>
#include <ustl/option.hpp>
#include <ustl/sync/atomic.hpp>

#include <ktl/name.hpp>
#include <gktl/canary.hpp>

namespace ours::object {
    class Dispatcher: public ustl::RefCounter<Dispatcher> {
        using Self = Dispatcher;
        using Base = ustl::RefCounter<Dispatcher>;
      public:
        auto koid() const -> KoId { 
            return m_koid;
        }

        auto get_lock() -> sync::Mutex & { 
            return m_lock;
        }

        auto increase_handle_count() -> void { 
            m_handle_count.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
        }

        auto decrease_handle_count() -> bool { 
            return m_handle_count.fetch_sub(1, ustl::sync::MemoryOrder::AcqRel) == 1;
        }

        auto attach_observer(SignalObserver *, void const *handle, Signals) -> Status;

        auto detach_observer(SignalObserver *, Signals *) -> Status;

        virtual auto on_zero_handles() -> void {}
      protected:
        auto notify_observers_locked(Signals signals) -> void;

        Dispatcher(u32 magic);
        virtual ~Dispatcher() = default;

        gktl::Canary m_canary;
        KoId m_koid;
        sync::Mutex m_lock;
        ustl::sync::AtomicU32 m_handle_count;
        ustl::sync::Atomic<Signal> m_signals;

        SignalObserver::List m_observers;
    };

    template <typename Derived, Rights DefaultRights, Signals ExtraSignals = Signals::None>
    class SoloDispatcher: public Dispatcher {
        using Base = Dispatcher;
        using Self = SoloDispatcher;
      public:
        CXX11_CONSTEXPR
        static auto default_right() -> Rights {
            return DefaultRights;
        }

        SoloDispatcher() 
            : Base(kMagic<Derived>)
        {}

        ~SoloDispatcher() = default;

        auto verify_canary() const -> void {
            m_canary.verify(kMagic<Derived>);
        }
    };

    template <typename Derived, Rights DefaultRights, Signals ExtraSignals = Signals::None>
    class PeeredDispatcher: public Dispatcher {
        using Base = Dispatcher;
        using Self = PeeredDispatcher;
      public:
        CXX11_CONSTEXPR
        static auto default_right() -> Rights {
            return DefaultRights;
        }

        auto verify_canary() const -> void {
            m_canary.verify(kMagic<Derived>);
        }

        auto on_zero_handles() -> void final {
        }
    };

} // namespace ours

#endif // #ifndef OURS_OBJECT_DISPATCHER_HPP
