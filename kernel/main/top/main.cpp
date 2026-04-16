#include <ours/start.hpp>
#include <ours/init.hpp>
#include <ours/cpu-mask.hpp>
#include <ours/task/thread.hpp>
#include <ours/task/scheduler.hpp>

#include <ours/mem/cpu-local.hpp>

/// Init calls
#include <ours/mem/init.hpp>
#include <ours/irq/init.hpp>
#include <ours/task/init.hpp>
#include <ours/time/init.hpp>
#include <ours/platform/init.hpp>

#include <ours/phys/handoff.hpp>

#include <logz4/log.hpp>

#include <gktl/init_hook.hpp>
#include <gktl/static_objects.hpp>

#include <ours/platform/acpi.hpp>

using ours::task::Thread;
using gktl::InitLevel;

namespace ours {
namespace {
    INIT_CODE
    auto labour_routine() -> i32 {
        init_arch();
        set_init_level(InitLevel::Arch);

        init_platform();
        set_init_level(InitLevel::Platform);

        // Ok, now there is no any routine to access the `init` area.
        mem::reclaim_init_area();

        // The 2nd stage has finished, and next is to load userboot.

        auto const gibberish = [] (char const *msg) {
            while (1) {
                log::debug("I'm {}", msg);
                for (int i = 0; i < 10000; ++i);
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
} // namespace anonymous

    /// Called from arch-code.
    /// Note: Invoke it after finishing the collection to early architecture 
    /// specific information passed from `PhysBoot`.
    ///
    /// Assumptions:
    ///     1). The early memory allocator has been initialized.
    NO_MANGLE INIT_CODE
    auto start_kernel(PhysAddr handoff) -> void {
        gktl::init_static_objects();

        setup_handoff(handoff);

        // The dynamic CPU-Local allocator for BP will be installed. 
        mem::CpuLocal::init_early();

        // Make Thread::Current work.
        task::init_early();

        init_arch_early();
        set_init_level(InitLevel::ArchEarly);

        init_platform_early();
        set_init_level(InitLevel::PlatformEarly);

        mem::init_vmm();
        set_init_level(InitLevel::Vmm);

        mem::CpuLocal::init();
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