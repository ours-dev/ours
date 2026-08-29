#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ustl/fmt/format_to.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    constexpr char const *LEVEL_NAMES[static_cast<usize>(DpcPriority::Count)] = {
        "Low",
        "Normal",
        "High",
    };
} // namespace

auto DpcRunner::Queue::start(char const *name, CpuNum cpu) -> Status {
    m_thread = task::Thread::spawn(name, task::DEFAULT_PRIORITY, &Queue::do_work, this);
    if (!m_thread) {
        return Status::NoResource;
    }

    m_thread->set_cpu_affinity_hard(CpuMask::of(cpu));
    m_thread->detach();
    m_thread->resume();
    return Status::Ok;
}

auto DpcRunner::Queue::enqueue(Dpc &dpc, CpuNum cpu, Level level) -> void {
    Guard guard{m_lock};

    DEBUG_ASSERT(Dpc::State::Idle == DpcRunner::state_of(dpc), "Dpc is already queued or running");

    DpcRunner::mark_queued(dpc, cpu, level);
    m_list.push_back(dpc);
    m_event.signal();
}

auto DpcRunner::Queue::cancel(Dpc &dpc) -> Status {
    Guard guard{m_lock};

    auto const state = DpcRunner::state_of(dpc);
    if (Dpc::State::Running == state) {
        return Status::BadState;
    }
    if (Dpc::State::Idle == state) {
        return Status::NotFound;
    }

    m_list.erase(dpc);
    DpcRunner::mark_idle(dpc);
    return Status::Ok;
}

auto DpcRunner::Queue::do_work() -> i32 {
    while (true) {
        [[maybe_unused]]
        auto const status = m_event.wait();
        DEBUG_ASSERT(Status::Ok == status);

        Dpc *next = nullptr;
        {
            Guard guard{m_lock};

            auto front = m_list.front();
            if (front.is_none()) {
                m_event.unsignal();
                continue;
            }

            next = &front.value();
            m_list.pop_front();
            DpcRunner::mark_running(*next);
        }

        next->invoke();

        {
            Guard guard{m_lock};
            DpcRunner::mark_idle(*next);
        }
    }
    return 0;
}

auto DpcRunner::init() -> void {
    auto &runner = Current::get();
    auto const cpu = mm::CpuLocal::current_cpu();

    for (auto i = usize(0); i < static_cast<usize>(Level::Count); ++i) {
        char name[24];
        ustl::fmt::format_to(name, "Dpc/{}/{}\0", cpu, LEVEL_NAMES[i]);

        [[maybe_unused]]
        auto const status = runner.m_dpc_queues[i].start(name, cpu);
        DEBUG_ASSERT(Status::Ok == status, "Failed to start Dpc worker[{}] on cpu {}",
                     LEVEL_NAMES[i], cpu);
    }
}

auto DpcRunner::enqueue(Dpc &dpc, Level level) -> Status {
    auto const cpu = mm::CpuLocal::current_cpu();
    auto &runner = Current::get();
    runner.m_dpc_queues[static_cast<usize>(level)].enqueue(dpc, cpu, level);
    return Status::Ok;
}

auto DpcRunner::cancel(Dpc &dpc) -> Status {
    auto &runner = Current::get(Self::owner_cpu_of(dpc));
    auto &queue = runner.m_dpc_queues[static_cast<usize>(Self::owner_level_of(dpc))];
    return queue.cancel(dpc);
}

} // namespace ours
