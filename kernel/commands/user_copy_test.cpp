/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `usercopy.test`: run the user-copy paths against a real user address space,
// and report every check with the values it saw.
//
// It builds a user address space of its own, attaches the calling thread to
// it for the duration, maps three pages of anonymous memory without committing
// them, and unmaps the third, which leaves
//
//     P0, P1  mapped, not yet present -- the first touch faults, and the fault
//             can be resolved
//     HOLE    not mapped at all -- the fault cannot be resolved
//
// and drives both kinds of copy across them:
//
//   - The capturing copies must hand every fault back unresolved, with the
//     exact address that faulted and the right cause; must leave the page as
//     absent as they found it; must do all of that while a spin lock is held
//     with interrupts off, which is the whole reason they exist; and must
//     refuse, without a fault, what they never attempt -- an address outside
//     the user half, an element count that overflows. A copy that starts in a
//     present page and runs into an absent one must report the absent page's
//     first byte, not the start of the buffer.
//   - soft_fault() on a captured fault must make the page present, and the
//     retried copy must then succeed and round-trip; on the hole it must fail.
//   - The resolving copies must demand-page P1 in place -- the successful
//     demand-paging path, which a normal boot never reaches, because every
//     fault the boot takes is on an address that cannot be resolved -- and
//     must still fail cleanly on the hole.
//
// Why a console command: the tree has no in-kernel unittest runtime yet (see
// //kernel/products/bin:vmours.with-tests), and what is under test is a page
// fault the MMU raises on a user mapping, which no host test can produce.
//
// Why its own address space: kshell runs a command on its own kernel thread,
// which has no user address space to borrow, and mapping scratch pages into
// some process's would be reaching into memory the test does not own. A kernel
// thread resolves a user-address fault against its own m_aspace (see
// Thread::Current::fault()), so once attached, the fault path, soft_fault()
// and the copies all see the test's address space and nothing else.
//
// Why "usercopy" and not "user_copy": gktl's command table encodes '-' and '.'
// but not '_' (CommandKeyEncoder in kernel/lib/gktl/src/command.cpp), so a
// name with an underscore is never found and the command silently does not
// run.
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using mm::VmfCause;

    constexpr auto WRITE_ABSENT = VmfCause::Write | VmfCause::Absent;
    constexpr auto READ_ABSENT = VmfCause::Absent;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("usercopy.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("usercopy.test: FAIL  {}", name);
    }

    /// What a capturing copy came back with, printed before its verdict so
    /// that the log is evidence and not only a list of passes.
    auto show(char const *const name, UserCopyCaptureResult const &result) -> void {
        if (result.fault.has_value()) {
            log::info("usercopy.test:   {}: status={} fault va={:#x} cause={:#x}", name,
                      static_cast<int>(result.status), result.fault->va,
                      static_cast<usize>(result.fault->cause));
            return;
        }
        log::info("usercopy.test:   {}: status={} no fault", name, static_cast<int>(result.status));
    }

    auto faulted_at(UserCopyCaptureResult const &result, VirtAddr const va, VmfCause const cause)
        -> bool {
        if (result.status != Status::InvArgs || !result.fault.has_value()) {
            return false;
        }
        return result.fault->va == va && result.fault->cause == cause;
    }

    auto refused_without_fault(UserCopyCaptureResult const &result) -> bool {
        return result.status == Status::InvArgs && !result.fault.has_value();
    }

    auto succeeded(UserCopyCaptureResult const &result) -> bool {
        return result.status == Status::Ok && !result.fault.has_value();
    }

    auto run_checks(Tally &tally, VirtAddr const p0, VirtAddr const p1, VirtAddr const hole)
        -> void {
        constexpr u64 PATTERN = 0x5afe'c0de'1234'5678;
        constexpr u64 OTHER = 0x0bad'f00d'8765'4321;

        // Refusals that must not claim a fault: nothing was attempted, so
        // there is nothing a caller could resolve.
        {
            u64 kernel_word = 0;
            auto const kernel_va = reinterpret_cast<VirtAddr>(&kernel_word);
            auto const r = user_copy::copy_to_user_capture_faults(kernel_va, &PATTERN, 8);
            show("kernel address", r);
            verdict(tally, refused_without_fault(r), "a kernel address is refused, no fault");
        }
        {
            auto const r = user_copy::copy_to_user_capture_faults(hole, &PATTERN, 0);
            show("empty copy", r);
            verdict(tally, succeeded(r), "an empty copy succeeds without touching anything");
        }
        {
            auto const r = user_copy::UserOutPtr<u64>(p0).copy_array_to_user_capture_faults(
                &PATTERN, ~usize(0));
            show("overflowing count", r);
            verdict(tally, refused_without_fault(r),
                    "an element count that overflows is refused, no fault");
        }

        // Capture hands the fault back, precisely, and resolves nothing.
        {
            auto const r = user_copy::copy_to_user_capture_faults(p0, &PATTERN, 8);
            show("write to absent P0", r);
            verdict(tally, faulted_at(r, p0, WRITE_ABSENT),
                    "a write to an absent page is captured at its address, Write|Absent");
        }
        {
            u64 word = 0;
            auto const r = user_copy::copy_from_user_capture_faults(&word, p0, 8);
            show("read from P0 after capture", r);
            verdict(tally, faulted_at(r, p0, READ_ABSENT),
                    "capture resolved nothing: P0 is still absent, and a read says Absent");
        }

        // The reason capture exists: it works where resolving could not.
        {
            static sync::SpinLock lock;
            UserCopyCaptureResult r{.status = Status::Ok, .fault = {}};
            {
                Guard<sync::SpinLock, IrqSave> guard{lock};
                r = user_copy::copy_to_user_capture_faults(p1, &PATTERN, 8);
            }
            show("write to absent P1 under a spin lock, interrupts off", r);
            verdict(tally, faulted_at(r, p1, WRITE_ABSENT),
                    "capture under a spin lock with interrupts off returns the fault");
        }

        // soft_fault() resolves a captured fault, and the retry goes through.
        {
            auto const status = task::Thread::Current::soft_fault(p0, WRITE_ABSENT);
            log::info("usercopy.test:   soft_fault(P0): status={}", static_cast<int>(status));
            verdict(tally, status == Status::Ok, "soft_fault() resolves the captured fault on P0");
        }
        {
            auto const written = user_copy::copy_to_user_capture_faults(p0, &PATTERN, 8);
            u64 back = 0;
            auto const read = user_copy::copy_from_user_capture_faults(&back, p0, 8);
            show("retried write to P0", written);
            show("read back from P0", read);
            log::info("usercopy.test:   P0 holds {:#x}", back);
            verdict(tally, succeeded(written) && succeeded(read) && back == PATTERN,
                    "the retried copy succeeds, and the bytes round-trip");
        }

        // A copy that crosses from a present page into an absent one reports
        // the absent page's first byte -- which is what a caller retrying
        // with SoftFaultInRange() has to start from -- not the buffer's start.
        {
            u64 const pair[2] = {PATTERN, OTHER};
            auto const r = user_copy::copy_to_user_capture_faults(p1 - 8, pair, sizeof(pair));
            show("write straddling P0 into absent P1", r);
            verdict(tally, faulted_at(r, p1, WRITE_ABSENT),
                    "a straddling copy reports the first byte of the absent page");
        }

        // An address nothing maps is captured the same way, and soft_fault()
        // then cannot help: the caller learns it is a bad argument.
        {
            auto const r = user_copy::copy_to_user_capture_faults(hole, &PATTERN, 8);
            show("write to the hole", r);
            verdict(tally, faulted_at(r, hole, WRITE_ABSENT),
                    "a write to an unmapped page is captured at its address");
            auto const status = task::Thread::Current::soft_fault(hole, WRITE_ABSENT);
            log::info("usercopy.test:   soft_fault(hole): status={}", static_cast<int>(status));
            verdict(tally, status != Status::Ok, "soft_fault() on the hole fails");
        }

        // The resolving copies: P1 is still absent, and a plain copy has to
        // demand-page it in place and go on.
        {
            auto const written = user_copy::copy_to_user(p1, &OTHER, 8);
            u64 back = 0;
            auto const read = user_copy::copy_from_user(&back, p1, 8);
            log::info("usercopy.test:   plain copies to/from absent P1: {} {}, P1 holds {:#x}",
                      static_cast<int>(written), static_cast<int>(read), back);
            verdict(tally, written == Status::Ok && read == Status::Ok && back == OTHER,
                    "a plain copy demand-pages an absent page and round-trips");
        }
        {
            auto const status = user_copy::copy_to_user(hole, &OTHER, 8);
            log::info("usercopy.test:   plain copy to the hole: {}", static_cast<int>(status));
            verdict(tally, status == Status::InvArgs,
                    "a plain copy to the hole fails with InvArgs");
        }
    }

    /// Puts the calling thread on |next| and loads its page tables, and hands
    /// back what it was on before.
    ///
    /// Preemption is off across the two steps. The scheduler loads CR3 from
    /// thread->aspace() on every switch (Scheduler's context switch calls
    /// VmAspace::switch_aspace(curr->aspace(), next->aspace())), and the
    /// architecture's switch keeps per-aspace CPU bookkeeping keyed on which
    /// aspace it is leaving. A preemption between the two steps would have the
    /// scheduler and this code disagree about which one that is.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    auto usercopy_test_command(int, char **) -> int {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "usercopy.test", &aspace);
            status != Status::Ok) {
            log::error("usercopy.test: could not create an address space: {}",
                       static_cast<int>(status));
            return 1;
        }

        auto *const previous = attach(aspace.as_ptr_mut());

        Tally tally;
        // Not const: ustl::Arc passes its own constness through to the area,
        // and mapping and unmapping change it.
        auto root = aspace->root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_anon(mm::VmPlacement::anywhere(), 3 * PAGE_SIZE, flags,
                                      mm::VmMapOption::None, "usercopy.test");
        if (mapping.is_err()) {
            log::error("usercopy.test: could not map the scratch pages: {}",
                       static_cast<int>(mapping.unwrap_err()));
            tally.failed += 1;
        } else {
            auto const base = mapping.unwrap()->base();
            auto const p0 = base;
            auto const p1 = base + PAGE_SIZE;
            auto const hole = base + 2 * PAGE_SIZE;

            if (auto const status = root->unmap(hole, PAGE_SIZE); status != Status::Ok) {
                log::error("usercopy.test: could not punch the hole: {}", static_cast<int>(status));
                tally.failed += 1;
            } else {
                log::info("usercopy.test: P0={:#x} P1={:#x} hole={:#x}", p0, p1, hole);
                run_checks(tally, p0, p1, hole);
            }
        }

        // Back onto the thread's own address space before the test's goes, so
        // that nothing is left running on page tables that are being freed.
        attach(previous);
        if (auto const status = aspace->destroy(); status != Status::Ok) {
            log::error("usercopy.test: could not destroy the address space: {}",
                       static_cast<int>(status));
            tally.failed += 1;
        }

        if (tally.failed != 0) {
            log::error("usercopy.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("usercopy.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(UserCopyTest, "usercopy.test", ours::usercopy_test_command,
             "exercise capturing and resolving user copies on scratch user pages");
