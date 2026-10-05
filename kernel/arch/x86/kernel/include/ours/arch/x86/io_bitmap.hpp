/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_IO_BITMAP_HPP
#define OURS_ARCH_X86_IO_BITMAP_HPP 1

#include <ours/compiler.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/types.hpp>

#include <atomic>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours {
struct X86Tss;

/// The I/O ports ring 3 may use while one address space is loaded: the ports
/// its process opened with ioports_request, kept as sorted runs of port
/// numbers. One per address space (mm::ArchVmAspace), so one per process.
///
/// The same answer exists in two forms, and keeping them equal is this
/// class's whole job:
///
///  - the runs held here, written by open() and close(), which are the truth;
///  - each CPU's TSS bitmap (X86Tss::io_bitmap), one bit per port, which the
///    CPU consults on every in and out from ring 3. A clear bit lets the
///    access through with no trap, so the kernel never sees it: a bit left
///    clear for the wrong process hands that process the device.
///
/// A CPU's TSS opens the ports of the bitmap it loaded last, as they stood at
/// one version of it. load() runs at every switch to another address space:
/// it closes the ports of the one being left and opens those of the one
/// coming in, so a switch touches only the runs of the two processes rather
/// than all 8 KiB of the bitmap. A change made by open() or close() bumps the
/// version, and then, before returning, has every CPU that is running this
/// address space load the new version (reload(), through
/// IpiService::sync_exec()).
///
/// The version is what makes the switch safe while a change is on its way.
/// Between the change and the reload reaching it, a CPU's TSS still holds the
/// old version, and that CPU may switch away first. Closing the runs as they
/// now stand would then miss a port that was just closed -- it is no longer in
/// the runs -- and leave it open for whichever process runs next. So a CPU
/// records the version its TSS matches, and a switch away from a bitmap whose
/// version has moved on closes every port instead.
///
/// The lock is a spin lock taken with interrupts off by every one of its
/// users: open() and close() in thread context, load() in the context switch
/// under the run queue's lock, reload() in the interrupt that sync_exec()
/// sends. Nothing is taken under it, and it is never held while waiting for
/// another CPU: the reload is sent after it is dropped, since the CPUs that
/// answer take it with interrupts off.
class IoBitmap {
    using Self = IoBitmap;

  public:
    IoBitmap() = default;

    /// Frees the runs. No CPU has this bitmap loaded by then. An empty mask
    /// of running CPUs, which mm::ArchVmAspace::destroy() asserts, does not
    /// show it: a switch clears its CPU's bit before load() reads the bitmap
    /// being left. When the address space can be freed does. A process holds
    /// its address space to the end, and lives until each of its threads has
    /// gone through Thread::destroy(), a Dpc that runs on the thread's CPU
    /// after its final switch, by which time load() there has let go of this
    /// bitmap. A kernel test that attaches to an address space of its own
    /// switches off it before it lets it go.
    ~IoBitmap();

    /// Opens the ports [base, base + count) to ring 3 for every thread of the
    /// address space, on every CPU it runs on, by the time this returns.
    ///
    /// |running| is the address space's mask of the CPUs running it. Each CPU
    /// sets its bit there before it loads this bitmap, so a CPU that loaded
    /// the version before this change is in the mask when it is read, after
    /// the change, and is sent the reload.
    ///
    /// InvArgs for a range past the last port, OutOfMem when the run it may
    /// need cannot be allocated. A |count| of 0 opens nothing, and interrupts
    /// no CPU. Must be called with interrupts on: it waits for the other CPUs
    /// to reload.
    [[nodiscard]]
    auto open(u32 base, u32 count, std::atomic<CpuMask> const &running) -> Status;

    /// Closes the ports [base, base + count) again: the same as open(), the
    /// other way. Closing the middle of a run splits it in two, which is the
    /// run that may need allocating here.
    [[nodiscard]]
    auto close(u32 base, u32 count, std::atomic<CpuMask> const &running) -> Status;

    /// Makes this CPU's TSS open |next|'s ports and no others: closes those
    /// of the bitmap loaded before (all of them, if its version moved on since
    /// it was loaded), opens |next|'s, and records |next| and its version.
    ///
    /// Called by the address space switch (mm::ArchVmAspace::switch_context()),
    /// after this CPU's bit is set in |next|'s mask of running CPUs, by a
    /// caller that keeps itself on this CPU. Interrupts are kept off inside,
    /// whatever the caller has: see the definition.
    static auto load(IoBitmap &next) -> void;

    /// Whether |port| is open on this CPU right now, as its TSS says: for
    /// ioport.test, which compares the bitmap the CPU consults with the runs.
    /// The caller keeps itself on this CPU.
    [[nodiscard]]
    static auto open_here(u32 port) -> bool;

    /// Whether this CPU's TSS holds an older version of the bitmap it has
    /// loaded: a change has been made to that bitmap and its reload has not
    /// reached this CPU yet. For ioport.test, which holds the reload off with
    /// interrupts and switches away in that state, the window the version is
    /// there for. The caller keeps itself on this CPU.
    [[nodiscard]]
    static auto stale_here() -> bool;

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(IoBitmap);

    /// The ports [base, end), all open. Runs never overlap or touch, and are
    /// kept in order of base: a run that would touch another is merged into
    /// it. So one change adds at most one run, and that one is allocated
    /// before the lock is taken.
    struct Run {
        u32 base;
        u32 end;
        ustl::collections::intrusive::ListMemberHook<> hook;
        USTL_DECLARE_HOOK_OPTION(Run, hook, HookOption);
    };
    USTL_DECLARE_LIST(Run, RunList, Run::HookOption);

    enum class Change {
        Open,
        Close,
    };

    /// open() and close(), which differ only in the edit made to the runs.
    auto change(u32 base, u32 count, Change change, std::atomic<CpuMask> const &running) -> Status;

    /// The edits. |spare| is the run allocated beforehand; one that uses it
    /// sets it to null. Runs that the edit removes go on |freed|, to be freed
    /// once the lock is dropped.
    TA_REQUIRES(m_lock) auto open_runs(u32 base, u32 end, Run *&spare, RunList &freed) -> void;
    TA_REQUIRES(m_lock) auto close_runs(u32 base, u32 end, Run *&spare, RunList &freed) -> void;

    /// Clears, or sets, the bits of every run in |tss|.
    TA_REQUIRES(m_lock) auto open_in(X86Tss &tss) -> void;
    TA_REQUIRES(m_lock) auto close_in(X86Tss &tss) -> void;

    /// The task sync_exec() runs on each CPU running the address space after
    /// a change: if this CPU still has |context|'s bitmap loaded at an older
    /// version, its TSS is rebuilt from the current runs.
    static auto reload(void *context) -> void;

    sync::SpinLock m_lock;
    RunList m_runs TA_GUARDED(m_lock);
    /// Bumped by every change, so that a CPU can tell whether its TSS still
    /// matches the runs.
    u64 m_version TA_GUARDED(m_lock) = 0;
};
} // namespace ours

#endif // OURS_ARCH_X86_IO_BITMAP_HPP
