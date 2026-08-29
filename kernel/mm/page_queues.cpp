#include <ours/mm/page_queues.hpp>

#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/lockdep.hpp>

#include <ustl/rc.hpp>
#include <ustl/sync/atomic.hpp>

using ustl::sync::Atomic;
using ustl::sync::MemoryOrder;

namespace ours::mm {
    /// Held through Arc<> by PageQueues, so it has to be reference counted.
    class PageQueues::MruRunner
        : public ustl::RefCounted<MruRunner, ustl::sync::ThreadSafePolicy> {
        using Self = PageQueues::MruRunner;
      public:

      private:
        [[maybe_unused]]
        task::Thread *m_thread;
    };

    /// Held through Arc<> by PageQueues, so it has to be reference counted.
    class PageQueues::LruRunner
        : public ustl::RefCounted<LruRunner, ustl::sync::ThreadSafePolicy> {
        using Self = PageQueues::LruRunner;
      public:
        enum class State {
            RequestShutdown,
            Shutdown,
        };

        auto run() -> void;
      private:
        Atomic<State> m_state;
        [[maybe_unused]]
        task::Thread *m_thread;
    };

    auto PageQueues::LruRunner::run() -> void {
        while (State::Shutdown == m_state.load(MemoryOrder::Relaxed)) {
        
        }
    }

    PageQueues::PageQueues() = default;

    PageQueues::~PageQueues() = default;

    auto PageQueues::set_pinned(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};

        return set_queue_backlink_locked(page, object, pgoff, QueueType::Pinned);
    }

    auto PageQueues::set_anonymous(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};

        return set_queue_backlink_locked(page, object, pgoff, QueueType::Anonymous);
    }

    auto PageQueues::set_reclaimable(VmPage *page, VmCowPages *object, PgOff pgoff) -> void {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};

        return set_queue_backlink_locked(page, object, pgoff, mrugen_to_queue_type());
    }

    auto PageQueues::set_queue_backlink_locked(VmPage *page, VmCowPages *object, 
                                               PgOff pgoff, QueueType type) -> void {
        page->set_pgoff(pgoff);
        page->set_object(object);

        auto const index = static_cast<usize>(type);
        m_queues[index].push_back(*page);
        m_sizes[index].fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
    }

    auto PageQueues::rotate_reclaimable_queues(AgeReason reason) -> void {
        {
            Guard<sync::SpinLock, IrqSave> guard{get_lock()};
            m_mrugen.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);

            m_last_age_time = time::SteadyClock::now();
            m_last_age_reason = reason;
        }
    }

} // namespace ours::mm