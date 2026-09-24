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

    /// Whether the faulting instruction belongs to a user copy in progress:
    /// the running thread has a resume address (it is inside x86_user_copy),
    /// and the instruction is in the copy routine itself.
    ///
    /// Both halves, not the slot alone. On a CPU without SMAP an interrupt
    /// taken in the middle of a copy runs with the slot still set, and if its
    /// handler touched a user address it would pass for the copy -- and be
    /// sent to the copy's exit with the interrupt's stack, which is a jump to
    /// an arbitrary return address.
    ///
    /// Zircon counterpart: the page_fault_resume slot x86_pfe_handler() reads;
    /// the address check is this kernel's addition (design 4.6, as revised
    /// 2026-09-24).
    auto inside_user_copy(arch::IrqFrame const *const frame) -> bool {
        auto const resume = *task::X86Thread::current_data_fault_resume();
        auto const ip = frame->ip;
        return resume != 0 && ip >= reinterpret_cast<usize>(x86_user_copy_begin) &&
               ip < reinterpret_cast<usize>(x86_user_copy_end);
    }

    /// Sends a user copy whose fault could not be resolved to its fault exit,
    /// from where it returns failure and the system call InvArgs.
    ///
    /// Only ever asked after x86_handle_page_fault() has established that
    /// ring 0 faulted on a user address inside a copy. Nothing is logged: a
    /// program passing a bad pointer is ordinary, and the InvArgs it gets back
    /// says everything that happened.
    auto resume_user_copy(arch::IrqFrame *const frame) -> void {
        frame->ip = *task::X86Thread::current_data_fault_resume();
    }
} // namespace

using arch::Cr2;
using mm::VmfCause;

auto x86_handle_page_fault(arch::IrqFrame *frame) -> Status {
    usize const error_code = frame->error_code;

    VirtAddr const addr = Cr2::read().get<Cr2::Address>();

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
    // "potential SMAP failure" first when SMAP explains it. This kernel still
    // resolves ring-0 faults on its own addresses through page_fault(), so the
    // refusal covers user addresses only. A null dereference in the kernel
    // lands here as an access to user address 0, which says what happened
    // well enough.
    auto const supervisor = (error_code & PAGE_FAULT_USER) == 0;
    auto const user_address = !mm::is_kernel_address(addr);
    if (supervisor && user_address && !inside_user_copy(frame)) {
        auto const access_open = frame->flags.get<arch::ArchCpuState::AC>() != 0;
        if (feature_has_smap && !access_open) {
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

    // Call the top level page fault handling routine.
    auto const status = task::Thread::Current::page_fault(addr, cause);
    if (Status::Ok == status) {
        return Status::Ok;
    }

    // Not resolvable, and a user copy's: not the kernel's to die of. By the
    // check above, a ring-0 fault on a user address that got this far is
    // inside a copy.
    if (supervisor && user_address) {
        resume_user_copy(frame);
        return Status::Ok;
    }
    return status;
}

} // namespace ours
