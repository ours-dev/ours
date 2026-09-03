#include <ours/cpu_mask.hpp>
#include <ours/time/init.hpp>
#include <ours/time/timer_queue.hpp>

namespace ours::time {
/// Binds every CPU's TimerQueue from the BSP, before any AP is running.
///
/// This mirrors task::init() binding every Scheduler the same way. Doing it
/// per CPU on the AP's own path would be too late: sleeps and wait-queue
/// timeouts started on the BSP may target another CPU's queue before that CPU
/// has come up, and a queue that still says INVALID_CPU_NUM cannot tell whose
/// tick device to program.
INIT_CODE auto init() -> void {
    cpu_possible_mask().for_each([](CpuNum cpu) { TimerQueue::get(cpu).init(cpu); });
}

} // namespace ours::time
