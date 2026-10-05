/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef THREAD_SAMPLER_THREAD_SAMPLER_HPP
#define THREAD_SAMPLER_THREAD_SAMPLER_HPP 1

#include <atomic>

#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sampler.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/timer.hpp>
#include <prof/sampler.hpp>
#include <ustl/box.hpp>

/// The kernel half of the sampling profiler: what makes a sample happen, and
/// what a session owns.
///
/// //kernel/lib/prof records a sample it is handed and knows nothing of the
/// kernel. This decides when one is taken, by keeping a timer armed on every
/// CPU while a session runs and on none otherwise, and it allocates what a
/// session records into when the session is created, so a kernel nobody
/// profiles pays nothing for the profiler.
///
/// A kernel library rather than part of //kernel/kernel: it is a facility
/// user space asks for through a Sampler object, not something the kernel
/// needs for itself. //kernel/kernel/task calls into it (a marked thread
/// walks its user stack on its way out), and //kernel/object owns sessions
/// through SamplerDispatcher. The namespace is `sampler`, not `prof`, which
/// is the core library's.
namespace sampler {
/// The shortest period a session may sample at: the ABI's
/// SAMPLER_MIN_PERIOD, as a Duration.
inline constexpr auto MIN_PERIOD = ours::Nanoseconds(ours::SAMPLER_MIN_PERIOD);

/// The longest: the ABI's SAMPLER_MAX_PERIOD. A timer is armed at now plus
/// the period and forwarded by it, so an unbounded period overflows the
/// deadline and leaves the timer expiring for ever in the interrupt.
inline constexpr auto MAX_PERIOD = ours::Nanoseconds(ours::SAMPLER_MAX_PERIOD);

/// The most a session may keep per CPU: the ABI's SAMPLER_MAX_BUFFER_SIZE. A
/// bound on what one create can ask the allocator for, times the number of
/// possible CPUs. It has to be small enough on its own: when frames run out,
/// the kernel's allocator retries instead of failing
/// (docs/repairs/260908/heap.md item 3), so a request for most of memory
/// hangs the creator rather than answering NoMemory.
inline constexpr auto MAX_BUFFER_SIZE = usize{ours::SAMPLER_MAX_BUFFER_SIZE};

/// What a session is created with.
struct Config final {
    /// How often each CPU takes a sample; from MIN_PERIOD to MAX_PERIOD.
    ours::Duration period;
    /// Bytes each CPU keeps. Rounded down to whole records, at least one,
    /// at most MAX_BUFFER_SIZE.
    usize buffer_size;
    /// What is recorded, at least one target: kernel stacks, unwound in the
    /// timer interrupt, and user stacks, walked on the interrupted thread's
    /// way back to user space.
    ours::SamplerTargets targets;
};

/// One profiling session: the per-CPU buffers and the kernel unwind index it
/// records into, and the sampler that does the recording. At most one holds
/// the claim at a time, because the per-CPU sampling timers it drives are one
/// system resource; a released one may live on, unable to start, until a
/// read that got hold of it finishes.
///
/// The states, guarded by the control lock:
///   - Configured: created, or stopped. The buffers hold the last run's
///     samples, if any, and may be read.
///   - Running: every online CPU's timer is armed and recording into this
///     session's buffers.
///   - Released: the session holds no claim on the system's one session.
///     It starts that way, until create() claims it, and ends that way,
///     once release() has given the claim up so that another may be created.
///     Nothing can be started or read. After a release the buffers stay
///     until the destructor, for a reader that got hold of them before.
///
/// Whoever holds the Box owns the memory. Releasing and destroying are two
/// steps because the owner can be shared: a kernel object whose last handle
/// is closed releases at once, while a system call still running on another
/// thread may hold a reference, and the memory goes with the last of those.
class Session final {
  public:
    /// Claims the system's one session and allocates its buffers and its
    /// unwind index.
    ///
    /// InvArgs for a period below MIN_PERIOD or a buffer_size outside one
    /// record to MAX_BUFFER_SIZE; AlreadyExists while another session exists
    /// and has not been released; NoMemory when an allocation fails.
    [[nodiscard]]
    static auto create(Config const &config, ustl::Box<Session> *out) -> ours::Status;

    /// Releases the session first if its owner did not.
    ~Session();

    Session(Session const &) = delete;
    auto operator=(Session const &) -> Session & = delete;

    /// Empties the buffers and arms every online CPU's timer, which records
    /// into them until stop(). BadState unless Configured.
    [[nodiscard]]
    auto start() -> ours::Status;

    /// Disarms every CPU's timer. When this returns no CPU is writing a
    /// sample into this session, so the buffers may be read. BadState unless
    /// Running.
    [[nodiscard]]
    auto stop() -> ours::Status;

    /// Stops the session if it runs, and gives up the system's one session:
    /// a create may succeed from now on. A second release does nothing.
    auto release() -> void;

    /// How many bytes read() would write, into |out|. BadState unless
    /// Configured.
    [[nodiscard]]
    auto dump_size(usize *out) -> ours::Status;

    /// Writes the dump through |writer|: an ours::SamplerDumpHeader, then
    /// every CPU's retained ours::SamplerRecords in CPU order. |actual| gets the
    /// bytes written, or, when |capacity| is less than the dump, the size
    /// it needs, with BufferTooSmall. BadState unless Configured; whatever
    /// |writer| answers ends the copy and is returned, with |actual| at the
    /// bytes written before it.
    ///
    /// The copy runs without the control lock, since a writer that copies
    /// to user memory can fault and wait; the read counts itself in first,
    /// and start() refuses while one is counted in, so no buffer is emptied
    /// under the copy.
    [[nodiscard]]
    auto read(prof::Sampler::ByteWriter const &writer, usize capacity, usize *actual)
        -> ours::Status;

    /// The user target's second half, called by the thread the timer marked
    /// (Thread::Current::request_stack_sample()) on its way back to user
    /// space, with interrupts on: walks its user stack from |pc| and |fp|
    /// by frame pointers, then, with interrupts off, records it into the
    /// running session's buffer for this CPU. Nothing is recorded when no
    /// session runs any more, or when the one that runs is not the one whose
    /// |generation| marked the thread.
    static auto sample_user_stack(u64 pc, u64 fp, u64 generation) -> void;

  private:
    using Timers = ours::mm::CpuLocal::PerCpu<ours::time::Timer>;

    enum class State : u8 {
        Configured,
        Running,
        Released,
    };

    Session(ours::Duration period, ours::SamplerTargets targets);

    /// Allocates the buffers, their slots and, for the kernel target, the
    /// unwind index, and wires them into m_sampler. NoMemory if the buffers cannot be had; an index
    /// that cannot be built only costs depth, as the sampler degrades to
    /// one-frame records.
    [[nodiscard]]
    auto allocate(usize records_per_cpu) -> ours::Status;

    TA_REQUIRES(s_lock)
    auto stop_locked() -> void;

    /// The timer's handler: interrupt context, on the CPU that armed it.
    static auto on_sample_due(Session *self, ours::Duration period, ours::time::Timer *timer)
        -> ours::time::Timer::HandlerResult;

    /// sync_exec() tasks, run on every online CPU: arm and cancel that CPU's
    /// timer.
    static auto arm_this_cpu(void *context) -> void;
    static auto disarm_this_cpu(void *context) -> void;

    /// The control lock: serialises create, start, stop and release, of
    /// every session, against each other. A sleeping lock on purpose: start
    /// and stop hold it across IpiService::sync_exec(), which needs
    /// interrupts on, and create holds it across allocations. Nothing on the
    /// sampling path takes it.
    static ours::sync::Mutex s_lock;

    /// Whether a session exists that has not been released.
    static bool s_claimed TA_GUARDED(s_lock);

    /// The running session, for a user-stack writer to record into; null
    /// when none runs. Set by start() before any timer is armed, cleared by
    /// stop before its barrier, which is what makes the barrier cover the
    /// writers that read it: each reads it and writes with interrupts off.
    static std::atomic<Session *> s_active;

    /// One sampling timer per CPU, shared by every session in turn.
    /// Allocated by the first start() rather than at boot, so a kernel nobody
    /// profiles never has them, and kept after that: a Timer is a few words.
    static Timers s_timers TA_GUARDED(s_lock);

    ours::Duration const m_period;
    ours::SamplerTargets const m_targets;

    /// Which run this is, among every run of every session this boot
    /// starts: what the timer leaves on a thread it marks, and what
    /// sample_user_stack() checks against the running session. Drawn anew by
    /// each start(), not once per session: a stop and a start empty the
    /// buffers, and a mark the old run left on a thread must stay out of
    /// them. A number, not this pointer, since a later session can be
    /// allocated where an earlier one was freed. Written only by start(),
    /// while no timer of this session is armed and s_active does not name
    /// it; the s_active release and the arming IPIs publish it.
    u64 m_generation = 0;

    /// The next run's generation, from 1, so that 0 names none.
    static std::atomic<u64> s_next_generation;
    State m_state TA_GUARDED(s_lock) = State::Released;

    /// Reads copying out of the buffers now, which start() waits for by
    /// refusing.
    u32 m_readers TA_GUARDED(s_lock) = 0;

    /// The per-CPU buffer headers, and the slots they borrow: one block,
    /// records_per_cpu for each possible CPU in turn.
    ours::mm::CpuLocal::PerCpu<prof::SampleBuffer> m_buffers;
    prof::SampleRecord *m_slots = nullptr;

    /// The kernel's FDEs, sorted by start address, for the unwinder; null
    /// when the index could not be built.
    unwind::FdeIndex::Entry *m_fde_entries = nullptr;

    prof::Sampler m_sampler;
};
} // namespace sampler

#endif // #ifndef THREAD_SAMPLER_THREAD_SAMPLER_HPP
