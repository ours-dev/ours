#include <ours/arch/thread.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/mm/fault.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/thread.hpp>

#include <arch/x86/interrupt.hpp>
#include <arch/x86/macro/mmu.hpp>
#include <arch/x86/system.hpp>
#include <logz4/log.hpp>

// The instructions of x86_user_copy and x86_user_copy_smap, which are the only
// ones a user-side fault may be resumed from (user_copy.S).
extern "C" char const x86_user_copy_begin[];
extern "C" char const x86_user_copy_end[];

namespace ours {
namespace {
    constexpr usize PAGE_FAULT_PRESENT = 1u << 0;
    constexpr usize PAGE_FAULT_WRITE = 1u << 1;
    constexpr usize PAGE_FAULT_USER = 1u << 2;
    /// I/D. The CPU reports it only with EFER.NXE or CR4.SMEP set; NXE is on
    /// on every CPU this kernel runs (mmu.cpp), so a fetch is never mistaken
    /// for a read.
    constexpr usize PAGE_FAULT_INSTRUCTION = 1u << 4;

    /// Whether this fault is one a user copy in progress may take back: the
    /// running thread is inside x86_user_copy (its resume address is set),
    /// the faulting instruction is in the copy routine itself, and |addr|
    /// lies in the user operand the copy was admitted for.
    ///
    /// Each half closes a different hole:
    ///
    ///  - the instruction, because the state alone would also cover an
    ///    interrupt taken in the middle of a copy on a CPU without SMAP: if
    ///    its handler faulted on a user address it would pass for the copy,
    ///    and be sent to the copy's exit with the interrupt's stack -- a jump
    ///    to an arbitrary return address;
    ///  - the address, because a copy has two operands and only one is the
    ///    user's. A fault on the other one -- a null or wild kernel buffer --
    ///    is a kernel bug, and resuming it would turn the bug into an InvArgs
    ///    the system call returns without a word.
    ///
    /// Zircon counterpart: the page_fault_resume slot x86_pfe_handler() reads;
    /// both checks are this kernel's additions (design 4.6, as revised
    /// 2026-09-24).
    auto user_copy_may_resume(arch::IrqFrame const *const frame, VirtAddr const addr) -> bool {
        auto const &state = *task::X86Thread::current_user_copy();
        auto const ip = frame->ip;
        auto const in_routine = ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
                                ip < reinterpret_cast<usize>(x86_user_copy_end);
        auto const in_operand = addr - state.user_base < state.user_size;
        return state.resume != 0 && in_routine && in_operand;
    }

    /// Sends a user copy whose fault could not be resolved to its fault exit,
    /// from where it returns failure and the system call InvArgs.
    ///
    /// Only ever asked after x86_handle_page_fault() has established that
    /// ring 0 faulted on a user address a copy may resume from. Nothing is
    /// logged: a program passing a bad pointer is ordinary, and the InvArgs it
    /// gets back says everything that happened.
    auto resume_user_copy(arch::IrqFrame *const frame) -> void {
        frame->ip = task::X86Thread::current_user_copy()->resume;
    }

    /// What the exception entry itself has already added to the running
    /// thread's disable counts by the time this handler runs.
    ///
    /// arch_handle_exception() calls platform_start_handling_irq() ahead of
    /// the dispatch, and that reaches irq::handle_irq_preamble(), which does
    /// one disable_preemption() and one disable_eager_reschedule() -- the same
    /// thing Zircon's int_handler_start() does. So every fault, including one
    /// taken by ring-3 code that had disabled nothing, arrives reading one.
    /// Anything above this came from the faulting context itself.
    constexpr u8 PREAMBLE_DISABLE_DEPTH = 1;

    /// Whether this fault may be resolved in a context that is allowed to
    /// sleep, which is the same question as whether interrupts may come back
    /// on for the duration.
    ///
    /// Three conditions, all of which have to hold:
    ///
    ///  - The faulting context had interrupts on. Kernel code that cleared IF
    ///    did so to keep something off this CPU, and a fault taken inside it
    ///    must not quietly undo that. The frame is the only surviving record:
    ///    vector 14 goes through an interrupt gate (idt.cpp builds every entry
    ///    with type_attr 0x8E), so IF is already clear by the time this runs,
    ///    whatever the faulting context had. Linux reads the same bit from the
    ///    same place in do_user_addr_fault().
    ///
    ///  - No spin lock is held on this CPU. Sleeping under one deadlocks
    ///    outright: the lock is never released, and the thread that would
    ///    release it cannot be scheduled onto a CPU that is spinning for it.
    ///
    ///  - The faulting context had not disabled preemption of its own. Same
    ///    argument one step removed -- code that disabled it is relying on
    ///    staying on this CPU, and sleeping is exactly what would move it.
    ///    "Of its own" is the whole subtlety, and is why this compares against
    ///    PREAMBLE_DISABLE_DEPTH rather than zero.
    ///
    /// The last two together are Linux's faulthandler_disabled(); its
    /// in_atomic() folds both into one preempt counter, where this kernel
    /// keeps the spin-lock count and the preemption count separately.
    ///
    /// The eager-reschedule count is deliberately not consulted. It gates
    /// whether a reschedule *request* may be sent eagerly to other CPUs, not
    /// whether this thread may block, and the preamble raises it too.
    ///
    /// Answering no does not refuse the fault. It resolves the way every fault
    /// resolved before this: interrupts off, and only down paths that do not
    /// sleep. What answering yes buys is the right for VmAspace's lock to
    /// become a mutex, so that the fault path and VmObject's own operations
    /// finally serialise against each other on the VMO's page tree.
    auto may_sleep_resolving(arch::IrqFrame const *const frame) -> bool {
        if (frame->flags.get<arch::ArchCpuState::IF>() == 0) {
            return false;
        }
        if (sync::SpinLock::num_held_cpu() != 0) {
            return false;
        }

        // Against the preamble's own depth, not against zero. A fault that had
        // disabled nothing still reads one here, so comparing with zero would
        // refuse every fault there is -- which is what this did at first, and
        // the probe that caught it saw `preempt=1` on a plain ring-3 fault.
        //
        // `<=` rather than `==` so that the day the preamble stops raising
        // these counts, this keeps saying yes instead of silently inverting.
        auto const &preemption = task::Thread::Current::preemption_state();
        return preemption.preemption_disabled_count() <= PREAMBLE_DISABLE_DEPTH;
    }
} // namespace

using mm::VmfCause;

auto x86_handle_page_fault(arch::IrqFrame *frame, VirtAddr const addr) -> Status {
    usize const error_code = frame->error_code;

    // Ring 0 may touch a user address in exactly one place: inside a user
    // copy (user_copy.S). Anywhere else it is a kernel bug -- something
    // dereferenced a user pointer instead of copying through it -- and it is
    // refused here, before the fault is resolved, on every CPU.
    //
    // It used to be refused only with SMAP on and AC clear, which is how
    // SMAP happens to report it. Without SMAP the same bug went through
    // page_fault() and succeeded whenever the page could be supplied: the
    // kernel read or wrote whatever the program had mapped there, and nothing
    // said so. Whether a stray user access is caught cannot depend on the CPU
    // model -- Nehalem, the default QEMU model here, has no SMAP.
    //
    // On a SMAP CPU with the page present, handing it on would also hang:
    // page_fault() finds the PTE, answers Ok, and the instruction runs again
    // into the same fault forever -- which is what the shell's first
    // debug_write used to do on such a CPU.
    //
    // Zircon's x86_pfe_handler() dies on any ring-0 fault without a
    // page_fault_resume address ("page fault in kernel mode"), and prints
    // "potential SMAP failure" first when SMAP explains it. Here a ring-0
    // fault on a kernel address already stops the kernel without a check of
    // its own: page_fault() refuses every kernel address (NotFound), and the
    // caller panics on that. A user address is the one page_fault() may
    // resolve, and so the one that has to be refused here. A null
    // dereference in the kernel lands here as an access to user address 0,
    // which says what happened well enough -- inside a copy as well as
    // outside one, because address 0 is never in a copy's admitted user range
    // (the range check starts at USER_ASPACE_BASE).
    auto const supervisor = (error_code & PAGE_FAULT_USER) == 0;
    auto const user_address = !mm::is_kernel_address(addr);
    if (supervisor && user_address && !user_copy_may_resume(frame, addr)) {
        auto const access_open = frame->flags.get<arch::ArchCpuState::AC>() != 0;
        auto const in_routine = frame->ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
                                frame->ip < reinterpret_cast<usize>(x86_user_copy_end);
        if (task::X86Thread::current_user_copy()->resume != 0 && in_routine) {
            log::error("user copy faulted on {:#x}, outside the user range it was admitted for",
                       addr);
        } else if (feature_has_smap && !access_open) {
            log::error(
                "potential SMAP violation: ring 0 touched user address {:#x} outside user_copy",
                addr);
        } else {
            log::error("page fault in kernel mode on user address {:#x} outside user_copy", addr);
        }
        return Status::AccessDenied;
    }

    VmfCause cause{};
    cause |= (error_code & PAGE_FAULT_PRESENT) ? VmfCause::None : VmfCause::Absent;
    cause |= (error_code & PAGE_FAULT_WRITE) ? VmfCause::Write : VmfCause::None;
    cause |= (error_code & PAGE_FAULT_USER) ? VmfCause::User : VmfCause::None;
    // Translated, not judged here. A fetch from a present page is not always
    // a violation: after a protect() that added Executable the page tables
    // can still hold the old NX entry, and that fault is spurious. Only the
    // mapping knows which permissions the page is meant to have, so it is
    // VmMapping::fault() that refuses the fetch. Zircon's x86_pfe_handler()
    // likewise only maps PFEX_I to VMM_PF_FLAG_INSTRUCTION.
    cause |= (error_code & PAGE_FAULT_INSTRUCTION) ? VmfCause::Instruction : VmfCause::None;

    // Call the top level page fault handling routine, with interrupts on
    // wherever that is allowed.
    //
    // Resolving a fault is not a short piece of work -- it walks the address
    // space, it allocates, and it is about to start taking a sleepable lock on
    // the VMO -- and running all of it with interrupts masked delays every
    // timer tick and IPI on this CPU for as long as it takes. The gate cleared
    // IF on the way in, so nothing else will turn them back on.
    //
    // They go off again before returning rather than being left on. Everything
    // above this frame -- the dispatch in faults.cpp and the stub that called
    // it -- runs with interrupts off and unwinds expecting that, and the iret
    // at the end restores the faulting context's own IF from the frame either
    // way, so the window is exactly this call and no wider.
    //
    // Zircon's x86_pfe_handler() enables interrupts at the same point and for
    // the same reason; Linux's do_user_addr_fault() does it before it takes
    // mmap_lock.
    auto const sleepable = may_sleep_resolving(frame);
    if (sleepable) {
        arch::local_irq_enable();
    }

    auto const status = task::Thread::Current::page_fault(addr, cause);

    if (sleepable) {
        arch::local_irq_disable();
    }

    if (Status::Ok == status) {
        return Status::Ok;
    }

    // Not resolvable, and a user copy's: not the kernel's to die of. By the
    // check above, a ring-0 fault on a user address that got this far is
    // one a copy may resume from.
    if (supervisor && user_address) {
        resume_user_copy(frame);
        return Status::Ok;
    }
    return status;
}

} // namespace ours
