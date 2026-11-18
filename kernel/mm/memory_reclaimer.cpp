/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
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
    auto const status = thread->resume();
    m_started.store(status == Status::Ok, std::memory_order_release);
    return status;
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

auto MemoryReclaimer::request(PmZone const &zone) -> void {
    // Checked before the mark, not after. A mark nothing will take stays
    // set, and every later mark on that node then answers that a wake has
    // already gone out -- so a report dropped after marking would swallow the
    // wakes of all the reports after it. A Pause that begins after this check
    // finds a mark whose wake goes out anyway; the thread's take is refused
    // while the Pause lives (take_node_target()), and the last Pause to end
    // wakes it to take the mark then.
    if (!m_started.load(std::memory_order_acquire) || paused()) {
        return;
    }
    // The node records; the wake is this reclaimer's own. Every mark that
    // finds the record empty is followed by a signal, so a record that is not
    // empty always has a wake on its way to the thread, and the event keeps
    // a signal that finds the thread busy.
    if (find_node(zone.which_node())->mark_short(zone)) {
        m_signal.signal();
    }
}

MemoryReclaimer::Pause::Pause(MemoryReclaimer &reclaimer) : m_reclaimer(reclaimer) {
    m_reclaimer.m_pauses.fetch_add(1, std::memory_order_acq_rel);
    // Waits out a take that read paused() before the count above: once this
    // returns, every take that starts sees the Pause. A take that finished
    // first has its walk to run, which goes on (see reclaim_node_requests()).
    Guard<sync::SpinLock, IrqSave> guard{m_reclaimer.m_take_lock};
}

MemoryReclaimer::Pause::~Pause() {
    if (m_reclaimer.m_pauses.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_reclaimer.m_signal.signal();
    }
}

auto MemoryReclaimer::take_node_target(PmNode &node) -> usize {
    Guard<sync::SpinLock, IrqSave> guard{m_take_lock};
    if (paused()) {
        return 0;
    }
    return node.take_reclaim_target();
}

auto MemoryReclaimer::take() -> usize {
    return m_wanted.exchange(0, std::memory_order_acq_rel);
}

auto MemoryReclaimer::reclaim(usize const frames) -> usize {
    return reclaim(frames, node_online_mask());
}

auto MemoryReclaimer::reclaim_superseding(usize const frames) -> usize {
    auto _ = take();
    return reclaim(frames);
}

auto MemoryReclaimer::reclaim(usize const frames, NodeMask const &nodes) -> usize {
    // A pause, or nothing asked, does not wait out a walk already running.
    // The tests that pause the reclaimer must not block behind one.
    if (paused() || frames == 0) {
        return 0;
    }
    // One walk at a time. The page a walk chooses stays on its queue until
    // that walk frees it, and the only name it has meanwhile is its frame
    // pointer. A second walk can be given that pointer again after the frame
    // has been handed out once more, and then free the new page.
    Guard guard{m_reclaim_lock};
    if (paused()) {
        return 0;
    }
    return walk(frames, nodes);
}

auto MemoryReclaimer::walk(usize const frames, NodeMask const &nodes) -> usize {
    // Each call is given what is left, so none takes more than the rest.
    usize freed = 0;
    nodes.for_each([&freed, frames](NodeId nid) {
        freed += find_node(nid)->page_queues().dedup_zero_pages(frames - freed);
    });
    nodes.for_each([&freed, frames](NodeId nid) {
        freed += find_node(nid)->page_queues().reclaim(frames - freed);
    });
    return freed;
}

auto MemoryReclaimer::reclaim_node_requests() -> void {
    for_each_node(NodeState::Memory, [this](PmNode &node) {
        auto const wanted = take_node_target(node);
        if (wanted == 0) {
            return;
        }
        // Walked whatever happens next. Through reclaim() a Pause beginning
        // now would refuse the walk, and the record, already taken, would be
        // lost; a walk whose record was taken is a walk already under way,
        // which a Pause does not stop.
        usize freed = 0;
        {
            Guard guard{m_reclaim_lock};
            freed = walk(wanted, NodeMask::of(node.nid()));
        }
        log::debug("mm: node {} gave back {} of {} frames requested", node.nid(), freed, wanted);
    });
}

auto MemoryReclaimer::run() -> i32 {
    while (true) {
        auto _ = m_signal.wait(time::Deadline::infinite());
        auto const wanted = take();
        if (wanted != 0) {
            auto const freed = reclaim(wanted);
            log::info("mm: gave back {} of {} frames that were asked", freed, wanted);
        }
        // Each node's take asks whether a Pause lives (take_node_target()). A
        // mark it leaves keeps the record non-empty with its wake spent, so
        // the last Pause to end signals again, and this takes it then.
        reclaim_node_requests();
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
