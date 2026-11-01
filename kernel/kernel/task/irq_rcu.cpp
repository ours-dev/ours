/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/ipi_service.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/irq_rcu.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

#include <arch/interrupt.hpp>

namespace ours::task {
namespace {
    /// What each CPU runs. It does nothing: that a CPU ran it, from its
    /// interrupt handler, is the fact the caller wants.
    auto rendezvous(void *) -> void {}
} // namespace

auto irq_rcu_synchronize() -> void {
    // Checked in the release image too: a call from inside a read would
    // return while the read is open, and free what its frames still use.
    ASSERT(!arch::irqs_disabled());
    ASSERT(Thread::Current::preemption_state().is_preemption_enabled());
    DEBUG_ASSERT(!Thread::Current::get()->is_idle_thread());
    DEBUG_ASSERT(cpu_hotplug_lock().holder() != Thread::Current::get());

    // The CPUs that go or come with the lock held stay as they are until the
    // end; see cpu_hotplug_lock().
    Guard<sync::Mutex> hotplug{cpu_hotplug_lock()};

    // The writer retracted what it is about to free with a store, and the
    // loop below starts with a load of the online mask. A CPU that joins
    // without the lock stores its bit and then loads what the readers find.
    // Without a full fence between the writer's two, both could read the old
    // value: the CPU not in the mask, and still reading the old object. The
    // joining CPU's half is Cpu::set_state(), which orders its store of the
    // bit before everything it reads after. This fence and that store are
    // what the argument needs; the loop below is belt over them.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Every CPU is asked once; the mask is read again after each round for
    // any that joined meanwhile, until a round finds none new.
    auto asked = CpuMask{};
    for (;;) {
        auto const pending = Cpu::online_mask() & ~asked;
        if (pending.none()) {
            return;
        }
        IpiService::sync_exec(pending, &rendezvous, nullptr);
        asked |= pending;
    }
}
} // namespace ours::task
