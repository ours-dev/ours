/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/observer.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/wait.hpp>
#include <ours/sync/event.hpp>
#include <ours/syscall.hpp>
#include <ours/time/timer.hpp>

#include <atomic>

namespace ours::object {
namespace {
    /// An observer that wakes one waiting thread.
    ///
    /// Lives on the waiter's stack for exactly as long as the wait. Both
    /// callbacks run under the observed object's lock, so they only record
    /// what happened and signal the event; the waiter does everything else
    /// once it is awake. Zircon's WaitSignalObserver.
    class WaitSignalObserver final: public SignalObserver {
      public:
        explicit WaitSignalObserver(sync::Event *const event) : m_event(event) {}

        auto on_match(Signals const) -> void override {
            m_event->signal(Status::Ok);
        }

        auto on_cancel(Signals const) -> void override {
            m_cancelled.store(true, std::memory_order_relaxed);
            m_event->signal(Status::Cancelled);
        }

        [[nodiscard]]
        auto cancelled() const -> bool {
            return m_cancelled.load(std::memory_order_relaxed);
        }

      private:
        sync::Event *const m_event;
        std::atomic<bool> m_cancelled{false};
    };
} // namespace

auto wait_one(HandleValue const handle, Signals const signals, Instant const deadline,
              Signals *const observed) -> Status {
    if (deadline != 0 && deadline != INSTANT_INFINITE) {
        return Status::NotSupported;
    }

    // Never null: the syscall layer refuses a trap from anything but a user
    // process's vDSO before a handler runs.
    auto *const process = ProcessDispatcher::current();

    sync::Event event;
    WaitSignalObserver observer(&event);
    ustl::Arc<Dispatcher> object;
    auto status = process->handle_table().attach_observer(handle, &observer, signals, &object);
    if (Status::Ok != status) {
        return status;
    }

    // A poll does not sleep: attaching already reported a condition that
    // holds (level-triggered), so all that is left is to read the state.
    auto result = Status::TimeOut;
    if (deadline == INSTANT_INFINITE) {
        // Interruptible, so that a thread being killed is not held here
        // forever by a peer that never writes.
        result = event.wait(time::Deadline::infinite());
    }

    // Detach whatever happened: after a match or a cancel the observer is
    // already unlinked and this only reads the signals; after an interrupted
    // sleep it is still linked and has to come off before this frame -- which
    // it lives in -- goes away.
    Signals current = Signals::None;
    static_cast<void>(object->detach_observer(&observer, &current));

    if (observer.cancelled()) {
        *observed = current | Signals::HandleClosed;
        return Status::Cancelled;
    }
    *observed = current;
    if (!!(current & signals)) {
        return Status::Ok;
    }
    // TimeOut for a poll that found nothing. Or Ok, from a match whose signal
    // was cleared again before this thread got to look: Zircon reports the
    // signals asserted now in that case too, so the caller acts on the state
    // as it is rather than as it briefly was.
    return result;
}
} // namespace ours::object
