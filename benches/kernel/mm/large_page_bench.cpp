/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `largepage.bench`: what mapping an object with 2 MiB leaves changes, against
// the same object mapped with 4 KiB pages (.agents/tasks/large-pages, step 6).
//
// It maps a 64 MiB object of each kind -- plain, and VmoFlags::LargePages --
// into an address space of its own, and measures:
//
//   faults        a byte written to every page, the first pass a program
//                 makes over fresh memory: how many page faults that takes.
//                 4 KiB: one per page; 2 MiB: one per block.
//   first touch   how long that pass takes. The work besides the faults is
//                 the same for both -- the same pages zeroed, the same copies
//                 -- so the difference is what the faults cost.
//   page tables   the tables below the 2 MiB entries: the ones in the tree,
//                 plus the ones set aside to split a leaf with. Expected the
//                 same for both, since a leaf keeps the very table its 4 KiB
//                 mapping would have used.
//   committed     a byte written to one page in every 2 MiB, the access a
//                 sparse structure makes: how much memory the object holds
//                 afterwards. The cost of large pages: a whole block for each
//                 page touched.
//   random reads  reads of pages chosen at random, all present: what the TLB
//                 reach of a 2 MiB leaf saves. Needs the processor's TLB, so
//                 it is skipped on an emulator (bench::Needs::Hardware).
//
// Every fault is taken by a real copy into the user mapping, and captured
// rather than resolved in place, so that each one is counted; it is then
// resolved by soft_fault(), the handler a user fault reaches, and the copy
// redone.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/page_table_deposits.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <bench/run.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize LARGE = 2_mb;
    constexpr usize SIZE = 64_mb;
    constexpr usize WINDOWS = SIZE / LARGE;
    constexpr usize RANDOM_READS = usize{1} << 16;

    /// What one kind of object measured.
    struct Numbers {
        usize faults = 0;
        u64 touch_ns = 0;
        /// Page tables below 2 MiB entries, in the tree.
        usize tables = 0;
        /// Page tables set aside to split a leaf with.
        usize set_aside = 0;
        usize leaves = 0;
        usize sparse_pages = 0;
        u64 reads_ns = 0;
    };

    /// Puts the calling thread on |next| and loads its page tables, and hands
    /// back what it was on before. Preemption is off across the two steps for
    /// the reason usercopy.test's attach() gives.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    /// An address space of the bench's own, the calling thread on it, and an
    /// object of |SIZE| mapped into it read-write where the VMM places it.
    class Scratch final {
      public:
        auto open(mm::VmoFlags const flags) -> Status {
            if (auto const status = mm::VmAspace::create(
                    USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                    mm::VmasFlags::None, "largepage.bench", &m_aspace);
                status != Status::Ok) {
                return status;
            }
            m_previous = attach(m_aspace.as_ptr_mut());
            if (auto const status = mm::VmObjectPaged::create(mm::ALLOC_USER, SIZE, flags, &m_vmo);
                status != Status::Ok) {
                return status;
            }
            auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
            auto mapped = m_aspace->root_vma()->map_with_vmo(
                VirtAddr::Anywhere, SIZE, 0, rw, mm::VmaFlags::PermMask, m_vmo,
                mm::VmMapOption::None, "largepage.bench");
            if (mapped.is_err()) {
                return mapped.unwrap_err();
            }
            m_base = mapped.unwrap()->base();
            return Status::Ok;
        }

        /// Back onto the thread's own address space, and the bench's
        /// destroyed, with everything mapped into it.
        auto close() -> Status {
            if (m_previous != nullptr) {
                attach(m_previous);
                m_previous = nullptr;
            }
            m_vmo = {};
            if (!m_aspace) {
                return Status::Ok;
            }
            auto const status = m_aspace->destroy();
            m_aspace = {};
            return status;
        }

        [[nodiscard]]
        auto base() const -> VirtAddr {
            return m_base;
        }

        [[nodiscard]]
        auto aspace() -> mm::VmAspace & {
            return *m_aspace;
        }

        [[nodiscard]]
        auto vmo() -> mm::VmObjectPaged & {
            return *m_vmo;
        }

      private:
        ustl::Arc<mm::VmAspace> m_aspace;
        mm::VmAspace *m_previous = nullptr;
        ustl::Arc<mm::VmObjectPaged> m_vmo;
        VirtAddr m_base = VirtAddr{0};
    };

    /// Writes a byte at every |stride| bytes of the mapping, and adds the
    /// page faults that takes to |*faults|.
    auto touch(VirtAddr const base, usize const stride, usize *const faults) -> Status {
        u8 const byte = 1;
        for (auto va = base; va < base + SIZE; va += stride) {
            while (true) {
                auto const copied = user_copy::copy_to_user_capture_faults(va, &byte, 1);
                if (copied.fault.is_none()) {
                    if (copied.status != Status::Ok) {
                        return copied.status;
                    }
                    break;
                }
                *faults += 1;
                if (auto const status =
                        task::Thread::Current::soft_fault(copied.fault->va, copied.fault->cause);
                    status != Status::Ok) {
                    return status;
                }
            }
        }
        return Status::Ok;
    }

    /// Counts how each 2 MiB window of the mapping is translated: by a leaf,
    /// or by a page table of 4 KiB entries.
    auto count_windows(Scratch &scratch, Numbers &numbers) -> void {
        for (usize window = 0; window < WINDOWS; ++window) {
            PhysAddr pa{};
            mm::MmuFlags flags{};
            usize size = 0;
            auto const va = scratch.base() + window * LARGE;
            if (scratch.aspace().arch_aspace().query(va, &pa, &flags, &size) != Status::Ok) {
                continue;
            }
            if (size == LARGE) {
                numbers.leaves += 1;
            } else {
                numbers.tables += 1;
            }
        }
    }

    /// Reads 8 bytes at each of RANDOM_READS pages picked at random, the same
    /// pages in the same order on every call, and answers how long that took.
    auto random_reads(VirtAddr const base, u64 *const elapsed_ns) -> Status {
        // xorshift64: cheap next to a read, and a fixed seed gives both kinds
        // the same sequence.
        u64 state = 0x9e37'79b9'7f4a'7c15;
        u64 sink = 0;
        auto const started = time::SteadyClock::now();
        for (usize i = 0; i < RANDOM_READS; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            auto const page = state % (SIZE / PAGE_SIZE);
            u64 word = 0;
            if (auto const status = user_copy::copy_from_user(&word, base + page * PAGE_SIZE, 8);
                status != Status::Ok) {
                return status;
            }
            sink += word;
        }
        *elapsed_ns = static_cast<u64>(Nanoseconds(time::SteadyClock::now() - started).count());
        // The pages all hold 1 in their first byte, so the sum is known; using
        // it keeps the loop from being anything but reads.
        if (sink != RANDOM_READS) {
            return Status::InternalError;
        }
        return Status::Ok;
    }

    /// Everything but the sparse pass, on an object made with |flags|.
    auto measure_dense(mm::VmoFlags const flags, bool const reads, Numbers &numbers) -> Status {
        Scratch scratch;
        auto status = scratch.open(flags);
        auto const held_before = mm::PageTableDeposits::nr_held();
        if (status == Status::Ok) {
            auto const started = time::SteadyClock::now();
            status = touch(scratch.base(), PAGE_SIZE, &numbers.faults);
            numbers.touch_ns =
                static_cast<u64>(Nanoseconds(time::SteadyClock::now() - started).count());
        }
        if (status == Status::Ok) {
            numbers.set_aside = mm::PageTableDeposits::nr_held() - held_before;
            count_windows(scratch, numbers);
        }
        if (status == Status::Ok && reads) {
            status = random_reads(scratch.base(), &numbers.reads_ns);
        }
        if (auto const closed = scratch.close(); status == Status::Ok) {
            status = closed;
        }
        return status;
    }

    /// The sparse pass: one page in every 2 MiB touched.
    auto measure_sparse(mm::VmoFlags const flags, Numbers &numbers) -> Status {
        Scratch scratch;
        auto status = scratch.open(flags);
        usize faults = 0;
        if (status == Status::Ok) {
            status = touch(scratch.base(), LARGE, &faults);
        }
        if (status == Status::Ok) {
            numbers.sparse_pages = scratch.vmo().committed_pages(0, SIZE);
        }
        if (auto const closed = scratch.close(); status == Status::Ok) {
            status = closed;
        }
        return status;
    }

    auto measure(char const *const kind, mm::VmoFlags const flags, bool const reads,
                 Numbers &numbers) -> bool {
        if (auto const status = measure_dense(flags, reads, numbers); status != Status::Ok) {
            log::error("largepage.bench: {}: the dense pass failed: {}", kind, to_string(status));
            return false;
        }
        if (auto const status = measure_sparse(flags, numbers); status != Status::Ok) {
            log::error("largepage.bench: {}: the sparse pass failed: {}", kind, to_string(status));
            return false;
        }
        return true;
    }

    auto largepage_bench_command(int const argc, char **const argv) -> int {
        bench::Run const run("largepage.bench", argc, argv);
        if (!run.arguments_ok()) {
            log::warn("usage: largepage.bench [all]");
            return 1;
        }
        // Asked before anything is measured, so that a skipped metric is
        // skipped for both kinds alike.
        auto const reads = run.measures("random reads", bench::Needs::Hardware);

        Numbers small;
        Numbers large;
        if (!measure("4K", mm::VmoFlags::Lazy, reads, small) ||
            !measure("2M", mm::VmoFlags::Lazy | mm::VmoFlags::LargePages, reads, large)) {
            return 1;
        }

        log::info("largepage.bench: {} MiB, a byte written to every page", SIZE / 1_mb);
        log::info("largepage.bench:   faults        4K {:>8}     2M {:>8}", small.faults,
                  large.faults);
        log::info("largepage.bench:   first touch   4K {:>8} us  2M {:>8} us",
                  small.touch_ns / 1000, large.touch_ns / 1000);
        log::info("largepage.bench:   2 MiB leaves  4K {:>8}     2M {:>8}", small.leaves,
                  large.leaves);
        log::info(
            "largepage.bench:   page tables   4K {:>8}     2M {:>8}  (in the tree + set aside)",
            small.tables + small.set_aside, large.tables + large.set_aside);
        log::info("largepage.bench: {} MiB, a byte written to one page in every 2 MiB",
                  SIZE / 1_mb);
        log::info("largepage.bench:   committed     4K {:>8} KiB 2M {:>8} KiB",
                  small.sparse_pages * PAGE_SIZE / 1_kb, large.sparse_pages * PAGE_SIZE / 1_kb);
        if (reads) {
            log::info("largepage.bench: {} reads of a random page, every page present",
                      RANDOM_READS);
            log::info("largepage.bench:   per read      4K {:>8} ns  2M {:>8} ns",
                      small.reads_ns / RANDOM_READS, large.reads_ns / RANDOM_READS);
        }
        return run.finish();
    }
} // namespace
} // namespace ours

GKTL_COMMAND(LargePageBench, "largepage.bench", ours::largepage_bench_command,
             "largepage.bench [all]: 4 KiB pages against 2 MiB leaves on a 64 MiB object; `all` "
             "runs the hardware-only metrics on an emulator too");
