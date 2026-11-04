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
    if (!m_started.load(std::memory_order_acquire) || paused()) {
        return;
    }
    auto const bit = usize{1} << std::to_underlying(zone.zone_type());
    auto const pending =
        m_pending_zones[zone.which_node()].fetch_or(bit, std::memory_order_acq_rel);
    if ((pending & bit) == 0) {
        m_signal.signal();
    }
}

auto MemoryReclaimer::take() -> usize {
    return m_wanted.exchange(0, std::memory_order_acq_rel);
}

auto MemoryReclaimer::reclaim(usize const frames) -> usize {
    return reclaim(frames, PmNode::node_online_mask());
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
    // Each call is given what is left, so none takes more than the rest.
    usize freed = 0;
    nodes.for_each([&freed, frames](NodeId nid) {
        freed += PmNode::node(nid)->page_queues().dedup_zero_pages(frames - freed);
    });
    nodes.for_each([&freed, frames](NodeId nid) {
        freed += PmNode::node(nid)->page_queues().reclaim(frames - freed);
    });
    return freed;
}

auto MemoryReclaimer::reclaim_node_requests() -> void {
    PmNode::for_each_nid(PmNode::State::Memory, [this](NodeId nid) {
        auto const zones = m_pending_zones[nid].exchange(0, std::memory_order_acq_rel);
        if (zones == 0) {
            return;
        }

        usize wanted = 0;
        for (usize type = 0; type < NR_ZONES_PER_NODE; ++type) {
            if ((zones & (usize{1} << type)) == 0) {
                continue;
            }
            auto const &zone = *PmZone::at(nid, ZoneType(type));
            auto const free = zone.free_frames();
            auto const high = zone.watermark(PmZone::WaterMark::High);
            if (free < high) {
                wanted += high - free;
            }
        }
        if (wanted != 0) {
            auto const freed = reclaim(wanted, NodeMask::of(nid));
            log::debug("mm: node {} gave back {} of {} frames requested", nid, freed, wanted);
        }
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
