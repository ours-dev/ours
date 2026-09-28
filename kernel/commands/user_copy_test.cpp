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
//   - VmObjectPaged::read_user()/write_user() must move bytes straight
//     between a VMO page and a user buffer under the VMO's lock, resolving a
//     user-side fault with the lock dropped: into an absent page, across a
//     fault that strikes part-way through, from a user page into the VMO, not
//     at all into a hole -- and into a mapping of the very VMO being read,
//     which is the case a copy that resolved faults in place would deadlock on.
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
#include <ours/mm/gaf.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
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
        if (result.fault.is_some()) {
            log::info("usercopy.test:   {}: status={} fault va={:#x} cause={:#x}", name,
                      static_cast<int>(result.status), result.fault->va,
                      static_cast<usize>(result.fault->cause));
            return;
        }
        log::info("usercopy.test:   {}: status={} no fault", name, static_cast<int>(result.status));
    }

    auto faulted_at(UserCopyCaptureResult const &result, VirtAddr const va, VmfCause const cause)
        -> bool {
        if (result.status != Status::InvArgs || result.fault.is_none()) {
            return false;
        }
        return result.fault->va == va && result.fault->cause == cause;
    }

    auto refused_without_fault(UserCopyCaptureResult const &result) -> bool {
        return result.status == Status::InvArgs && result.fault.is_none();
    }

    auto succeeded(UserCopyCaptureResult const &result) -> bool {
        return result.status == Status::Ok && result.fault.is_none();
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

    /// VmObjectPaged::read_user() and write_user(): the bytes go between a VMO
    /// page and a user buffer in one copy, under the VMO's lock, and a fault on
    /// the user side is captured, resolved with the lock dropped, and retried.
    ///
    /// The VMO holds A B at offset 0, C just before its second page and D just
    /// after it. The user buffers are fresh scratch pages, absent until touched,
    /// and a hole; and for the last check, a mapping of the VMO itself.
    auto run_vmo_checks(Tally &tally, ustl::Arc<mm::VmArea> &root) -> void {
        constexpr u64 A = 0x1111'2222'3333'4444;
        constexpr u64 B = 0x5555'6666'7777'8888;
        constexpr u64 C = 0x9999'aaaa'bbbb'cccc;
        constexpr u64 D = 0xdddd'eeee'ffff'0000;
        constexpr u64 E = 0x0123'4567'89ab'cdef;

        ustl::Arc<mm::VmObjectPaged> vmo;
        if (auto const status =
                mm::VmObjectPaged::create(mm::GAF_USER, 2 * PAGE_SIZE, mm::VmoFlags{}, &vmo);
            status != Status::Ok) {
            log::error("usercopy.test: could not create the VMO: {}", static_cast<int>(status));
            tally.failed += 1;
            return;
        }
        u64 const head[2] = {A, B};
        u64 const seam[2] = {C, D};
        if (vmo->write(head, 0, sizeof(head)) != Status::Ok ||
            vmo->write(seam, PAGE_SIZE - 8, sizeof(seam)) != Status::Ok) {
            log::error("usercopy.test: could not fill the VMO");
            tally.failed += 1;
            return;
        }

        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto scratch = root->map_anon(mm::VmPlacement::anywhere(), 3 * PAGE_SIZE, flags,
                                      mm::VmMapOption::None, "usercopy.test buffers");
        if (scratch.is_err()) {
            log::error("usercopy.test: could not map the VMO test buffers: {}",
                       static_cast<int>(scratch.unwrap_err()));
            tally.failed += 1;
            return;
        }
        auto const q0 = scratch.unwrap()->base();
        auto const q1 = q0 + PAGE_SIZE;
        auto const qhole = q0 + 2 * PAGE_SIZE;
        if (root->unmap(qhole, PAGE_SIZE) != Status::Ok) {
            log::error("usercopy.test: could not punch the VMO test hole");
            tally.failed += 1;
            static_cast<void>(root->unmap(q0, 3 * PAGE_SIZE));
            return;
        }
        log::info("usercopy.test: VMO buffers Q0={:#x} Q1={:#x} hole={:#x}", q0, q1, qhole);

        // Q0 is absent, so the copy faults with the VMO's lock held: this is
        // the capture, drop, soft_fault(), retake, retry round trip.
        {
            auto const status = vmo->read_user(user_copy::UserOutPtr<u8>(q0), 0, 16);
            u64 got[2] = {};
            auto const back = user_copy::copy_from_user(got, q0, sizeof(got));
            log::info(
                "usercopy.test:   read_user(VMO[0..16) -> absent Q0): {}, Q0 holds {:#x} {:#x}",
                static_cast<int>(status), got[0], got[1]);
            verdict(tally, status == Status::Ok && back == Status::Ok && got[0] == A && got[1] == B,
                    "read_user into an absent user page resolves it and delivers the bytes");
        }

        // VMO [PAGE-8, PAGE+8) into user [Q1-8, Q1+8): the first 8 bytes land
        // in Q0, present now; the next 8 fault on Q1. The retry has to start
        // from the 9th byte, with the cursor rebuilt at the VMO's second page.
        {
            auto const status =
                vmo->read_user(user_copy::UserOutPtr<u8>(q1 - 8), PAGE_SIZE - 8, 16);
            u64 got[2] = {};
            auto const back = user_copy::copy_from_user(got, q1 - 8, sizeof(got));
            log::info(
                "usercopy.test:   read_user(VMO seam -> Q0 end + absent Q1): {}, got {:#x} {:#x}",
                static_cast<int>(status), got[0], got[1]);
            verdict(tally, status == Status::Ok && back == Status::Ok && got[0] == C && got[1] == D,
                    "a read_user that faults part-way resumes where it stopped");
        }

        // The other direction: E, put in Q0 by a plain copy, into VMO offset 8.
        {
            auto const put = user_copy::copy_to_user(q0, &E, sizeof(E));
            auto const status = vmo->write_user(user_copy::UserInPtr<u8 const>(q0), 8, 8);
            u64 got = 0;
            auto const back = vmo->read(&got, 8, sizeof(got));
            log::info("usercopy.test:   write_user(Q0 -> VMO[8..16)): {}, VMO holds {:#x}",
                      static_cast<int>(status), got);
            verdict(tally,
                    put == Status::Ok && status == Status::Ok && back == Status::Ok && got == E,
                    "write_user moves a user page's bytes into the VMO");
        }

        {
            auto const status = vmo->read_user(user_copy::UserOutPtr<u8>(qhole), 0, 8);
            log::info("usercopy.test:   read_user(VMO -> hole): {}", static_cast<int>(status));
            verdict(tally, status == Status::InvArgs,
                    "read_user into an unmapped page fails with InvArgs");
        }

        // What the bounce page existed to avoid: the user buffer is a mapping
        // of the very VMO being read. VMO page 0 is copied into VMO page 1
        // through that mapping, whose page tables are empty, so the copy faults
        // while this VMO's lock is held -- and resolving that fault means
        // looking up this same VMO. Resolved in place it would deadlock as soon
        // as the fault path takes the VMO's lock; captured, it is resolved with
        // the lock dropped. A regression here shows up as a command that never
        // finishes.
        {
            auto self = root->map_with_vmo(mm::VmPlacement::anywhere(), 2 * PAGE_SIZE, 0, flags,
                                           mm::VmaFlags::PermMask, ustl::Arc<mm::VmObject>(vmo),
                                           mm::VmMapOption::None, "usercopy.test self");
            if (self.is_err()) {
                log::error("usercopy.test: could not map the VMO: {}",
                           static_cast<int>(self.unwrap_err()));
                tally.failed += 1;
            } else {
                auto const m = self.unwrap()->base();
                auto const status = vmo->read_user(user_copy::UserOutPtr<u8>(m + PAGE_SIZE), 0, 16);
                u64 got[2] = {};
                auto const back = vmo->read(got, PAGE_SIZE, sizeof(got));
                log::info("usercopy.test:   read_user(VMO[0..16) -> its own mapping at {:#x}): {}, "
                          "VMO[PAGE..] holds {:#x} {:#x}",
                          m + PAGE_SIZE, static_cast<int>(status), got[0], got[1]);
                verdict(
                    tally, status == Status::Ok && back == Status::Ok && got[0] == A && got[1] == E,
                    "read_user into a mapping of the same VMO neither deadlocks nor loses bytes");
                static_cast<void>(root->unmap(m, 2 * PAGE_SIZE));
            }
        }

        static_cast<void>(root->unmap(q0, 3 * PAGE_SIZE));
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

        run_vmo_checks(tally, root);

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
