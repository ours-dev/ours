#include <ours/boot/handoff.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/start.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

#include <userabi/userboot.hpp>

/// Init calls
#include <ours/irq/init.hpp>
#include <ours/mm/early_mem.hpp>
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
    /// Not INIT_CODE, and it is the one function on the boot path that must
    /// not be. It reclaims the `.init*` sections partway through and then
    /// keeps running -- spawning threads, returning -- so its own instructions
    /// have to sit outside the range it hands back. Marked INIT_CODE, as it
    /// was, this function was freeing the code it was executing from. Nothing
    /// faults at the free itself -- the mappings survive it -- so the window
    /// stays open until the first allocation hands one of those frames back
    /// out and writes over instructions still to be returned through; the
    /// Thread::spawn() below, whose kernel stack is the next allocation, is
    /// the obvious candidate. The boot has never got this far (it stops
    /// earlier, in the profiler's .eh_frame indexing), so that is the shape of
    /// the hazard, not an observed failure.
    ///
    /// Same split as Linux: the `__init` work is reached through calls
    /// (init_arch/init_platform, as `kernel_init_freeable()`), while the
    /// function that outlives the reclaim is not `__init` itself
    /// (`kernel_init()`, marked `__ref`).
    auto labour_routine(void *) -> i32 {
        init_arch();
        set_init_level(InitLevel::Arch);

        init_platform();
        set_init_level(InitLevel::Platform);

        // Both init_arch() and init_platform() have returned, so every
        // INIT_CODE frame this thread ever entered is gone and nothing below
        // reaches into the `.init*` sections: the CpuLocal template there is
        // only ever a source to copy from (see cpu_local.cpp's canonical_base),
        // and the gibberish lambda spawned below is emitted into plain .text.
        // This function itself is not INIT_CODE, which is the other half of
        // the condition -- see the note on its definition.
        mm::EarlyMem::reclaim_init_area();

        // The 2nd stage has finished. Bring up userboot.
        //
        // Here and not earlier: it reads the kernel package out of the
        // handoff and allocates whole frames for the decompressed copy, so
        // it needs the frame allocator, and it will shortly need to create
        // threads. It is also deliberately after reclaim_init_area(), so
        // that the pages it takes come from the pool the reclaim has already
        // returned rather than competing with it.
        userabi::userboot_init();

        auto const gibberish = [](void *arg) -> i32 {
            auto const msg = static_cast<char const *>(arg);
            while (1) {
                log::debug("I'm {}", msg);
                for (int i = 0; i < 10000; ++i)
                    ;
            }
            return 0;
        };

        auto t1 = Thread::spawn("Sample1", gibberish, const_cast<char *>("Thread 1"),
                                task::DEFAULT_PRIORITY);
        t1->detach();
        t1->resume();

        auto t2 = Thread::spawn("Sample2", gibberish, const_cast<char *>("Thread 2"),
                                task::DEFAULT_PRIORITY);
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

extern "C" INIT_CODE
auto start_kernel(PhysAddr handoff) -> void {
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

    auto const laborer = Thread::spawn("laborer", labour_routine, nullptr, task::DEFAULT_PRIORITY);
    laborer->set_cpu_affinity_hard(CpuMask::of(0));
    laborer->detach();
    laborer->resume();

    // Let the main thread become an idle thread.
    Thread::Current::become_idle();
}

/// The tail of an application CPU's bring-up, reached from the arch entry --
/// on x86, x86_start_nonboot_cpu() -- once that CPU is running on its own
/// per-CPU area, GDT, IDT, TSS and local APIC. This is Linux's
/// start_secondary(), and it ends the same way: publish the CPU, then fall
/// into the idle loop the first reschedule preempts.
///
/// Everything that another CPU could have prepared already has been. The boot
/// CPU built this CPU's Scheduler and idle thread in task::init(), and its
/// TimerQueue in time::init(), both walking cpu_possible_mask() -- because a
/// sleep or a wait-queue timeout started on the boot CPU may name a CPU that
/// has not booted yet, and a queue that still says INVALID_CPU_NUM cannot
/// tell whose tick device to program. What is left here is only what this CPU
/// must do for itself.
extern "C" [[noreturn]]
auto start_nonboot_cpu() -> void {
    // The half of task::init() that only the CPU it concerns can run; the boot
    // CPU runs it from init() itself.
    task::init_percpu();

    // Published last, and deliberately: `Online` is what x86_bring_aps()
    // spins on before it starts the next CPU and reuses the single trampoline
    // data page, and what Scheduler::select_target_cpu() falls back to when
    // no scheduler is Active anywhere. Setting it any earlier would advertise
    // a CPU that cannot yet take a thread.
    Cpu::set_state(Cpu::State::Online);
    log::info("CPU[{}] is online", Cpu::id());

    Thread::Current::become_idle();
}
} // namespace ours
