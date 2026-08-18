#include <ours/arch/x86/faults.hpp>
#include <ours/mm/fault.hpp>
#include <ours/task/thread.hpp>

#include <arch/x86/interrupt.hpp>
#include <arch/x86/macro/mmu.hpp>
#include <arch/x86/system.hpp>

#include <logz4/log.hpp>

namespace ours {
namespace {
    constexpr usize kPageFaultPresent = 1u << 0;
    constexpr usize kPageFaultWrite = 1u << 1;
    constexpr usize kPageFaultUser = 1u << 2;
} // namespace

    using arch::Cr2;
    using mm::VmfCause;

    auto x86_handle_page_fault(arch::IrqFrame *frame) -> Status {
        usize const error_code = frame->error_code;

        VirtAddr const addr = Cr2::read().get<Cr2::Address>();

        VmfCause cause{};
        cause |= (error_code & kPageFaultPresent) ? VmfCause::None : VmfCause::Absent;
        cause |= (error_code & kPageFaultWrite) ? VmfCause::Write : VmfCause::None;
        cause |= (error_code & kPageFaultUser) ? VmfCause::User : VmfCause::None;

        // Call the top level page fault handling routine.
        auto const status = task::Thread::Current::page_fault(addr, cause);
        if (Status::Ok != status) {
            return status;
        }

        return Status::Ok;
    }

} // namespace ours
