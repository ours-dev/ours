#include <ours/object/dispatcher.hpp>

#include <ours/lockdep.hpp>

namespace ours::object {
    Dispatcher::Dispatcher(u32 magic)
        : m_canary(magic)
        , m_lock()
        , m_handle_count()
    {}

    auto Dispatcher::attach_observer(SignalObserver *observer, void const *handle,
                                     Signals signals) -> Status {
        m_canary.verify<Self>();
        DEBUG_ASSERT(observer);

        Guard guard{get_lock()};

        observer->m_handle = handle;
        observer->m_triggering_signals = signals;
        m_observers.push_back(*observer);

        return Status::Ok;
    }

    auto Dispatcher::detach_observer(SignalObserver *observer, Signals *signals) -> Status {
        m_canary.verify<Self>();
        DEBUG_ASSERT(observer);

        Guard guard{get_lock()};

        if (signals) {
            *signals = m_signals.load(ustl::sync::MemoryOrder::Acquire);
        }

        if (observer->m_managed_hook.is_linked()) {
            (void)m_observers.erase(m_observers.iterator_to(*observer));
        }

        return Status::Ok;
    }

    auto Dispatcher::notify_observers_locked(Signals signals) -> void {
        for (auto iter = m_observers.begin(), end = m_observers.end(); iter != end;) {
            auto &observer = *iter;
            if (!(observer.m_triggering_signals & signals)) {
                ++iter;
                continue;
            }
            iter = m_observers.erase(iter);
            observer.on_match(signals);
        }
    }
} // namespace ours::object