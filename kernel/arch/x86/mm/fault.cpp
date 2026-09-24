#include <ours/arch/thread.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/mm/fault.hpp>
#include <ours/mm/vmm.hpp>
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

    /// Whether this unresolved fault belongs to a user copy that can take it
    /// back, and if so, sends the copy to its fault exit.
    ///
    /// Four conditions, all needed:
    ///
    ///  - ring 0 faulted, because a ring-3 fault is the program's own and goes
    ///    to its process, never to a kernel copy;
    ///  - on a user address, because a fault on the kernel side of the copy is
    ///    a kernel bug and should stop the kernel;
    ///  - the running thread has a resume address, so it is inside a copy;
    ///  - the faulting instruction is in the copy routine itself. Without this
    ///    the slot alone would decide, and on a CPU without SMAP an interrupt
    ///    taken in the middle of a copy, whose handler faulted on a user
    ///    address, would be sent to the copy's exit -- with the interrupt's
    ///    stack, and so a jump to an arbitrary return address.
    ///
    /// Nothing is logged: a program passing a bad pointer is ordinary, and the
    /// system call returning InvArgs says everything that happened.
    ///
    /// Zircon counterpart: the page_fault_resume path at the end of
    /// x86_pfe_handler(), which checks the slot alone; the address check is
    /// this kernel's addition (design 4.6, as revised 2026-09-24).
    auto resume_user_copy(arch::IrqFrame *const frame, VirtAddr const addr, bool const supervisor)
        -> bool {
        if (!supervisor || mm::is_kernel_address(addr)) {
            return false;
        }
        auto const resume = *task::X86Thread::current_data_fault_resume();
        auto const ip = frame->ip;
        auto const in_copy = ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
                             ip < reinterpret_cast<usize>(x86_user_copy_end);
        if (resume == 0 || !in_copy) {
            return false;
        }
        frame->ip = resume;
        return true;
    }
} // namespace

using arch::Cr2;
using mm::VmfCause;

auto x86_handle_page_fault(arch::IrqFrame *frame) -> Status {
    usize const error_code = frame->error_code;

    VirtAddr const addr = Cr2::read().get<Cr2::Address>();

    // Ring 0 touched a user address with SMAP on and AC clear -- something
    // dereferenced a user pointer outside //kernel/lib/user_copy, which is the
    // only code that sets AC. On a SMAP CPU that is a kernel bug whatever the
    // page's state, and it has to be refused here rather than handed on. If
    // the page is present, page_fault() finds the PTE, answers Ok, and the
    // instruction runs again into the same fault, forever: a silent hang,
    // which is what the shell's first debug_write used to do on such a CPU.
    // If it is absent, the fault would be resolved and then retried into that
    // same hang.
    //
    // Zircon's x86_pfe_handler() (exceptions_c.cc) makes the same test --
    // supervisor, SMAP on, AC clear, user address -- and dies with "potential
    // SMAP failure". A null dereference in the kernel lands here too, as an
    // access to user address 0, which says what happened well enough.
    auto const supervisor = (error_code & PAGE_FAULT_USER) == 0;
    auto const access_open = frame->flags.get<arch::ArchCpuState::AC>() != 0;
    if (supervisor && feature_has_smap && !access_open && !mm::is_kernel_address(addr)) {
        log::error("potential SMAP violation: ring 0 touched user address {:#x} outside user_copy",
                   addr);
        return Status::AccessDenied;
    }

    VmfCause cause{};
    cause |= (error_code & PAGE_FAULT_PRESENT) ? VmfCause::None : VmfCause::Absent;
    cause |= (error_code & PAGE_FAULT_WRITE) ? VmfCause::Write : VmfCause::None;
    cause |= (error_code & PAGE_FAULT_USER) ? VmfCause::User : VmfCause::None;

    // Call the top level page fault handling routine.
    auto const status = task::Thread::Current::page_fault(addr, cause);
    if (Status::Ok == status) {
        return Status::Ok;
    }

    // Resolved or not, a user copy's fault is not the kernel's to die of.
    if (resume_user_copy(frame, addr, supervisor)) {
        return Status::Ok;
    }
    return status;
}

} // namespace ours
