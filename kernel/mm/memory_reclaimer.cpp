/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/init.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/task/thread.hpp>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::mm {

auto MemoryReclaimer::get() -> Self & {
    static Self RECLAIMER;
    return RECLAIMER;
}

auto MemoryReclaimer::start() -> Status {
    // Thread::spawn() takes a plain function: this one only finds the
    // reclaimer it was handed and runs it.
    auto *const thread = task::Thread::spawn(
        "memory-reclaimer",
        [](void *const self) -> i32 { return static_cast<Self *>(self)->run(); }, this,
        task::DEFAULT_PRIORITY);
    if (thread == nullptr) {
        return Status::OutOfMem;
    }
    auto _ = thread->detach();
    auto _ = thread->resume();
    return Status::Ok;
}

auto MemoryReclaimer::request(usize const frames) -> void {
    if (frames == 0 || paused()) {
        return;
    }
    auto wanted = m_wanted.load(std::memory_order_relaxed);
    while (wanted < frames &&
           !m_wanted.compare_exchange_weak(wanted, frames, std::memory_order_acq_rel)) {
    }
    m_signal.signal();
}

auto MemoryReclaimer::reclaim(usize const frames) -> usize {
    if (paused()) {
        return 0;
    }
    // Each call is given what is left, so none takes more than the rest.
    usize freed = 0;
    PmNode::for_each_node(PmNode::State::Online, [&freed, frames](PmNode &node) {
        freed += node.page_queues().dedup_zero_pages(frames - freed);
    });
    PmNode::for_each_node(PmNode::State::Online, [&freed, frames](PmNode &node) {
        freed += node.page_queues().reclaim(frames - freed);
    });
    return freed;
}

auto MemoryReclaimer::run() -> i32 {
    while (true) {
        auto _ = m_signal.wait(time::Deadline::infinite());
        auto const wanted = m_wanted.exchange(0, std::memory_order_acq_rel);
        if (wanted == 0) {
            continue;
        }
        auto const freed = reclaim(wanted);
        log::info("mm: gave back {} of {} frames that were asked", freed, wanted);
    }
}

namespace {
    /// The init hook takes a plain function, as the thread does.
    INIT_CODE
    auto start_memory_reclaimer() -> void {
        if (auto const status = MemoryReclaimer::get().start(); Status::Ok != status) {
            log::error("mm: no thread for the memory reclaimer: {}", to_string(status));
        }
    }
    // Once threads can be made, as the watchdog that asks it.
    GKTL_INIT_HOOK(MemoryReclaimer, start_memory_reclaimer, gktl::InitLevel::Threading + 1);
} // namespace

} // namespace ours::mm
