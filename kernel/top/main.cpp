#include <ours/boot/handoff.hpp>
#include <ours/cpu-mask.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu-local.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

/// Init calls
#include <ours/irq/init.hpp>
#include <ours/mm/init.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/init.hpp>
#include <ours/task/init.hpp>
#include <ours/time/init.hpp>

#include <ccrt/runtime.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

using gktl::InitLevel;
using ours::task::Thread;

namespace ours {
namespace {
    INIT_CODE
    auto labour_routine() -> i32 {
        init_arch();
        set_init_level(InitLevel::Arch);

        init_platform();
        set_init_level(InitLevel::Platform);

        // Ok, now there is no any routine to access the `init` area.
        mm::reclaim_init_area();

        // The 2nd stage has finished, and next is to load userboot.

        auto const gibberish = [](char const *msg) {
            while (1) {
                log::debug("I'm {}", msg);
                for (int i = 0; i < 10000; ++i)
                    ;
            }
            return 0;
        };

        auto t1 = Thread::spawn("Sample1", task::kDefaultPriority, gibberish, "Thread 1");
        t1->detach();
        t1->resume();

        auto t2 = Thread::spawn("Sample2", task::kDefaultPriority, gibberish, "Thread 2");
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
NO_MANGLE INIT_CODE 
auto start_kernel(PhysAddr handoff) -> void {
    ccrt::init_global_static_objects();

    boot::setup_handoff(handoff);

    // The dynamic CPU-Local allocator for BP will be installed.
    mm::CpuLocal::init_early();

    // Make Thread::Current work.
    task::init_early();

    init_arch_early();
    set_init_level(InitLevel::ArchEarly);

    init_platform_early();
    set_init_level(InitLevel::PlatformEarly);

    mm::init_vmm();
    set_init_level(InitLevel::Vmm);

    // CpuLocal::init() itself now runs inside init_pmm(), ahead of the slab that
    // depends on it; this only publishes the level for hooks ordered after it.
    set_init_level(InitLevel::CpuLocal);

    // TODO(SmallHuaZi): It seems like doing nothing. Consider to remove
    // it or give it more thing to do.
    irq::init();
    set_init_level(InitLevel::Irq);

    time::init();
    set_init_level(InitLevel::Time);

    task::init();
    set_init_level(InitLevel::Threading);

    auto const laborer = Thread::spawn("laborer", task::kDefaultPriority, labour_routine);
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
