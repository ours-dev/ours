#include <ours/boot/handoff.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/start.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

/// Init calls
#include <ccrt/runtime.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/irq/init.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/init.hpp>
#include <ours/task/init.hpp>
#include <ours/time/init.hpp>

using gktl::InitLevel;
using ours::task::Thread;

namespace ours {
namespace {
    INIT_CODE auto labour_routine() -> i32 {
        init_arch();
        set_init_level(InitLevel::Arch);

        init_platform();
        set_init_level(InitLevel::Platform);

        // Ok, now there is no any routine to access the `init` area.
        mm::EarlyMem::reclaim_init_area();

        // The 2nd stage has finished, and next is to load userboot.

        auto const gibberish = [](char const *msg) {
            while (1) {
                log::debug("I'm {}", msg);
                for (int i = 0; i < 10000; ++i)
                    ;
            }
            return 0;
        };

        auto t1 = Thread::spawn("Sample1", task::DEFAULT_PRIORITY, gibberish, "Thread 1");
        t1->detach();
        t1->resume();

        auto t2 = Thread::spawn("Sample2", task::DEFAULT_PRIORITY, gibberish, "Thread 2");
        t2->detach();
        t2->resume();

        return 0;
    }
} // namespace

/// Called from arch-code.
/// Note: Invoke it after finishing the collection to early architecture
/// specific information passed from `PhysBoot`.
///
/// Assumptions:
///     1). The early memory allocator has been initialized.
namespace {
    /// Raw COM1 write for boot-step markers; see the x86 arch handoff.
    auto com1_put(char const c) -> void {
        asm volatile("out %0, %1" : : "a"(c), "d"(u16{0x3F8}));
    }
} // namespace

NO_MANGLE INIT_CODE auto start_kernel(PhysAddr handoff) -> void {
    com1_put('C');
    ccrt::init_global_static_objects();
    com1_put('B');

    // Everything below logs through logz4; install the serial logger first
    // so early failures are visible.
    init_bootstrap_logging();
    com1_put('L');

    boot::setup_handoff(handoff);
    com1_put('H');

    // The dynamic CPU-Local allocator for BP will be installed.
    mm::CpuLocal::init_early();
    com1_put('U');

    // Make Thread::Current work.
    task::init_early();
    com1_put('T');

    init_arch_early();
    com1_put('X');
    set_init_level(InitLevel::ArchEarly);

    // Brings the page allocator up on its way through (platform_init_pmm).
    init_platform_early();
    set_init_level(InitLevel::PlatformEarly);

    // The three steps below used to happen inside init_pmm(), with their levels
    // published afterwards and in a different order -- so a hook registered on
    // one of them ran nowhere near the thing it named. They are driven here, in
    // dependency order, and each level is announced where it becomes true.

    // Per-CPU areas: allocated out of the page allocator, so no earlier; and
    // the slab below wants a per-CPU cache per MemCache, so no later.
    mm::CpuLocal::init();
    set_init_level(InitLevel::CpuLocal);

    // The kernel object heap: the object caches, and kmalloc on top of them.
    // (The VMM's own vmalloc heap is a later step, inside init_vmm().)
    mm::init_cache();
    set_init_level(InitLevel::Heap);

    mm::init_vmm();
    set_init_level(InitLevel::Vmm);

    // TODO(SmallHuaZi): It seems like doing nothing. Consider to remove
    // it or give it more thing to do.
    irq::init();
    set_init_level(InitLevel::Irq);

    time::init();
    set_init_level(InitLevel::Time);

    task::init();
    set_init_level(InitLevel::Threading);

    auto const laborer = Thread::spawn("laborer", task::DEFAULT_PRIORITY, labour_routine);
    laborer->set_cpu_affinity_hard(CpuMask::of(0));
    laborer->detach();
    laborer->resume();

    // Let the main thread become an idle thread.
    Thread::Current::become_idle();
}

NO_MANGLE [[noreturn]]
auto start_nonboot_cpu() -> void {

    Thread::Current::become_idle();
}
} // namespace ours
