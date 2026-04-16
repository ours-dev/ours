#include <ours/dpc.hpp>
#include <ours/lockdep.hpp>

namespace ours {
    auto DpcRunner::Queue::do_work() -> i32 {
        while (1) {
            auto const status = m_event.wait(); 
            DEBUG_ASSERT(Status::Ok == status);

            ustl::Option<Dpc> dpc_local;
            {
                Guard guard{m_lock};

                if (m_list.empty()) {
                    m_event.unsignal();
                    continue;
                }

                auto &dpc = m_list.front();
                m_list.pop_front();

                dpc_local.emplace(dpc);
            }
            dpc_local->invoke();
        }
        return 0;
    }

    auto DpcRunner::Queue::enqueue(Dpc &dpc) -> void {
        Guard guard{m_lock};
        m_list.push_back(dpc);
    }

    auto DpcRunner::enqueue(Dpc &dpc, Dpc::Level level) -> Status {
        auto &runner = DpcRunner::Current::get();
        auto &queue = runner.m_dpc_queues[usize(level)];

        queue.enqueue(dpc);
        return Status::Ok;
    }

} // namespace ours