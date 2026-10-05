/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/x86/io_bitmap.hpp>
#include <ours/arch/x86/percpu.hpp>
#include <ours/assert.hpp>
#include <ours/ipi_service.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>

#include <algorithm>

#include <arch/auto_interrupt_disabler.hpp>
#include <heap/new.hpp>

namespace ours {
namespace {
    /// What this CPU's TSS opens: the ports of |bitmap| as they stood at
    /// |version| of it, or none at all while |bitmap| is null.
    ///
    /// Touched only by its own CPU, with interrupts off -- written by
    /// IoBitmap::load() in the context switch and by IoBitmap::reload() in an
    /// interrupt, read by IoBitmap::stale_here() as well -- so it needs no
    /// lock. Starts zeroed, as every CPU-local does, which reads as "nothing
    /// loaded": the state init() leaves the TSS in, every port closed.
    struct Loaded {
        IoBitmap *bitmap;
        u64 version;
    };

    class CurrentLoaded: public mm::CpuLocal::Current<Loaded> {};

    auto this_tss() -> X86Tss & {
        return mm::CpuLocal::access(X86_PCPU).tss;
    }
} // namespace

IoBitmap::~IoBitmap() {
    m_runs.clear_and_dispose([](Run *const run) noexcept { delete run; });
}

auto IoBitmap::open(u32 const base, u32 const count, std::atomic<CpuMask> const &running)
    -> Status {
    return change(base, count, Change::Open, running);
}

auto IoBitmap::close(u32 const base, u32 const count, std::atomic<CpuMask> const &running)
    -> Status {
    return change(base, count, Change::Close, running);
}

/// The edit, under the lock, then the reload, without it.
///
/// The reload is what lets a change keep its promise: once this returns,
/// every CPU running the address space has its TSS match the new runs. When
/// the caller is one of the address space's own threads, as a system call's
/// is, its CPU is one of them and reloads itself (sync_exec() runs its share
/// here).
///
/// The caller may be preempted between the edit and the reload, and nothing
/// stale survives that either. A thread of another address space taking its
/// CPU switches the bitmap there, sees the version moved on, and closes every
/// port. A thread of the same address space taking it leaves that CPU in the
/// mask, which is read only after the edit, so the reload reaches it. And the
/// CPU the caller resumes on either loads the new version as it switches the
/// caller in, or was running the address space already and is in the mask.
auto IoBitmap::change(u32 const base, u32 const count, Change const change,
                      std::atomic<CpuMask> const &running) -> Status {
    if (base > X86Tss::NUM_IO_PORTS || count > X86Tss::NUM_IO_PORTS - base) {
        return Status::InvArgs;
    }
    if (count == 0) {
        return Status::Ok;
    }
    auto const end = base + count;

    // Allocated before the lock, which is held with interrupts off, where the
    // heap may not be entered. An edit uses at most one new run: opening
    // [base, end) where it touches no run, or closing it out of the middle of
    // one, which splits that run in two.
    auto *spare = new (mm::GAF_KERNEL) Run{};
    if (spare == nullptr) {
        return Status::OutOfMem;
    }
    RunList freed;
    {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        if (change == Change::Open) {
            open_runs(base, end, spare, freed);
        } else {
            close_runs(base, end, spare, freed);
        }
        m_version += 1;
    }
    delete spare;
    freed.clear_and_dispose([](Run *const run) noexcept { delete run; });

    // Read after the edit. A CPU sets its bit in |running| before it loads
    // this bitmap; if it loaded the old version, that happened before this
    // edit took the lock, so the bit is visible here and the CPU is sent the
    // reload. One that loads after the edit loads the new version itself.
    IpiService::sync_exec(running.load(std::memory_order_relaxed), &Self::reload, this);
    return Status::Ok;
}

/// The run [base, end) joins the runs: the first run that reaches |base| or
/// beyond either starts past |end|, and the new run goes in before it, or
/// touches the new range, and grows over it and over every later run it now
/// reaches.
auto IoBitmap::open_runs(u32 const base, u32 const end, Run *&spare, RunList &freed) -> void {
    auto run = m_runs.begin();
    while (run != m_runs.end() && run->end < base) {
        ++run;
    }
    if (run == m_runs.end() || run->base > end) {
        spare->base = base;
        spare->end = end;
        // The iterator to the new run: nothing here walks on from it.
        auto _ = m_runs.insert(run, *spare);
        spare = nullptr;
        return;
    }

    run->base = std::min(run->base, base);
    run->end = std::max(run->end, end);
    auto next = run;
    ++next;
    while (next != m_runs.end() && next->base <= run->end) {
        run->end = std::max(run->end, next->end);
        auto &swallowed = *next;
        next = m_runs.erase(next);
        freed.push_back(swallowed);
    }
}

/// The range [base, end) leaves the runs: of each run that overlaps it, the
/// part below |base| and the part from |end| on are kept, and a run with
/// nothing left goes. A run with something left on both sides is split in
/// two, and no other run can overlap the range then.
auto IoBitmap::close_runs(u32 const base, u32 const end, Run *&spare, RunList &freed) -> void {
    auto run = m_runs.begin();
    while (run != m_runs.end() && run->end <= base) {
        ++run;
    }
    while (run != m_runs.end() && run->base < end) {
        if (run->base < base && run->end > end) {
            spare->base = end;
            spare->end = run->end;
            run->end = base;
            auto after = run;
            ++after;
            // The iterator to the new run: nothing here walks on from it.
            auto _ = m_runs.insert(after, *spare);
            spare = nullptr;
            return;
        }
        if (run->base < base) {
            run->end = base;
            ++run;
        } else if (run->end > end) {
            run->base = end;
            ++run;
        } else {
            auto &emptied = *run;
            run = m_runs.erase(run);
            freed.push_back(emptied);
        }
    }
}

auto IoBitmap::open_in(X86Tss &tss) -> void {
    for (auto const &run: m_runs) {
        tss.open_ports(run.base, run.end);
    }
}

auto IoBitmap::close_in(X86Tss &tss) -> void {
    for (auto const &run: m_runs) {
        tss.close_ports(run.base, run.end);
    }
}

/// The two halves of a switch, each under its own bitmap's lock, never both
/// at once. Closing first leaves this CPU's TSS, for a moment, with every
/// port closed, which is safe whatever runs: nothing here returns to ring 3
/// in between.
///
/// Interrupts stay off from the first half to the end of the second, though
/// each lock is let go between them. The reload an interrupt runs reads and
/// writes the same TSS and record: arriving between the halves, it would find
/// the record still naming |previous|, and open its ports again just after
/// they were closed. The scheduler switches with interrupts off already; the
/// kernel tests that attach themselves to an address space only hold off
/// preemption, which keeps this on one CPU but lets the reload in.
auto IoBitmap::load(IoBitmap &next) -> void {
    ::arch::AutoInterruptDisabler const no_interrupts;
    auto &loaded = CurrentLoaded::get();
    auto &tss = this_tss();

    if (auto *const previous = loaded.bitmap; previous != nullptr) {
        Guard<sync::SpinLock, IrqSave> guard{previous->m_lock};
        if (loaded.version == previous->m_version) {
            previous->close_in(tss);
        } else {
            // Changed since it was loaded here, and the reload has not come
            // yet: the runs no longer say which ports this TSS opens.
            tss.close_all_ports();
        }
    }

    Guard<sync::SpinLock, IrqSave> guard{next.m_lock};
    next.open_in(tss);
    loaded = Loaded{.bitmap = &next, .version = next.m_version};
}

auto IoBitmap::open_here(u32 const port) -> bool {
    DEBUG_ASSERT(port < X86Tss::NUM_IO_PORTS, "port {:#x} past the bitmap", port);
    return this_tss().port_is_open(port);
}

/// Interrupts off for the record, as everywhere else it is read: a reload
/// arriving midway would change it under the comparison.
auto IoBitmap::stale_here() -> bool {
    ::arch::AutoInterruptDisabler const no_interrupts;
    auto const &loaded = CurrentLoaded::get();
    auto *const bitmap = loaded.bitmap;
    if (bitmap == nullptr) {
        return false;
    }
    Guard<sync::SpinLock, IrqSave> guard{bitmap->m_lock};
    return loaded.version != bitmap->m_version;
}

/// Rebuilt whole rather than patched: the TSS matches an older version, whose
/// runs are gone, so only "every port closed" is known to be a state the new
/// runs can be laid over. A change is rare, and this runs only on the CPUs
/// running its address space.
auto IoBitmap::reload(void *const context) -> void {
    auto &bitmap = *static_cast<Self *>(context);
    auto &loaded = CurrentLoaded::get();
    if (loaded.bitmap != &bitmap) {
        // Switched to another address space since it was sent; that switch
        // closed this one's ports.
        return;
    }

    Guard<sync::SpinLock, IrqSave> guard{bitmap.m_lock};
    if (loaded.version == bitmap.m_version) {
        // Switched away and back since the change, loading it then.
        return;
    }
    auto &tss = this_tss();
    tss.close_all_ports();
    bitmap.open_in(tss);
    loaded.version = bitmap.m_version;
}
} // namespace ours
