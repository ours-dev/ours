/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_SESSION_HPP
#define SAMPLER_SESSION_HPP 1

#include <sampler/kernel_unwinder.hpp>
#include <sampler/recorder.hpp>
#include <sampler/stack_sample_request.hpp>

#include <array>
#include <atomic>

#include <arch/stack_root.hpp>
#include <ktl/vec.hpp>
#include <ours/chrono.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sampler.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/timer.hpp>
#include <ustl/box.hpp>

/// The kernel half of the sampling profiler: what makes a sample happen, and
/// what a session owns.
///
/// The core (recorder.hpp, kernel_unwinder.hpp, frame_pointers.hpp, built
/// and tested on the host) turns a stack into addresses and stores them,
/// and knows nothing of the kernel. This decides when a sample is taken, by keeping a timer armed on every CPU while a session runs
/// and on none otherwise, and it allocates what a session records into when
/// the session is created, so a kernel nobody profiles pays nothing for the
/// profiler.
///
/// A kernel library rather than part of //kernel/kernel: it is a facility
/// user space asks for through a Sampler object, not something the kernel
/// needs for itself. //kernel/kernel/task calls into it (a marked thread
/// walks its user stack on its way out), and //kernel/object owns sessions
/// through SamplerDispatcher.
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
/// records into, and the sampler that does the recording. At most one exists
/// at a time, from the claim create() takes until the session is destroyed:
/// the per-CPU sampling timers it drives are one system resource, and its
/// buffers take up to MAX_BUFFER_SIZE for every possible CPU, which is what
/// the claim bounds. A released session still holds the claim, unable to
/// start, until a read that got hold of it finishes and the memory goes.
///
/// The states, guarded by the control lock:
///   - Configured: created, or stopped. The buffers hold the last run's
///     samples, if any, and may be read.
///   - Running: every online CPU's timer is armed and recording into this
///     session's buffers.
///   - Released: nothing can be started or read. The session starts that
///     way, until create() has built it, and ends that way after release().
///     After a release the buffers stay until the destructor, for a reader
///     that got hold of them before, and so does the claim (Claim).
///
/// Whoever holds the Box owns the memory. Releasing and destroying are two
/// steps because the owner can be shared: a kernel object whose last handle
/// is closed releases at once, while a system call still running on another
/// thread may hold a reference, and the memory and the claim go with the
/// last of those.
class Session final {
  public:
    /// Claims the system's one session and allocates its buffers and its
    /// unwind index.
    ///
    /// InvArgs for a period below MIN_PERIOD or a buffer_size outside one
    /// record to MAX_BUFFER_SIZE; AlreadyExists while another session exists,
    /// released or not, or another create is building one; NoMemory when an
    /// allocation fails.
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

    /// Stops the session if it runs, and ends it: nothing can start or read
    /// it from now on. A second release does nothing. The claim is not given
    /// back here but when the session is destroyed, once its memory is gone,
    /// which for a session no one else holds is at once.
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
    auto read(Recorder::ByteWriter const &writer, usize capacity, usize *actual) -> ours::Status;

    /// The user target's second half, called by the thread the timer marked
    /// (Thread::Current::request_stack_sample()) on its way back to user
    /// space, with interrupts on: walks its user stack from |pc| and |fp|
    /// by frame pointers, then, with interrupts off, records it into the
    /// running session's buffer for this CPU. Nothing is recorded when no
    /// session runs any more, or when the one that runs is not the one whose
    /// |generation| marked the thread.
    static auto sample_user_stack(u64 pc, u64 fp, u64 generation) -> void;

    /// The Timers level's CPU hotplug hook (ours/cpu_hotplug_hook.hpp): the
    /// CPU going offline cancels its own sampling timer before its tick
    /// stops, so the timer is not among those another CPU takes over; the CPU
    /// coming back arms it, once its tick runs again, if a session is
    /// running. On that CPU, interrupts off. No session starts or stops in
    /// between: start(), stop() and release() take the CPU hotplug lock
    /// first, which the CPU's change of state holds throughout.
    static auto on_cpu_offline() -> void;
    static auto on_cpu_online() -> void;

  private:
    using Timers = ours::mm::CpuLocal::PerCpu<ours::time::Timer>;

    enum class State : u8 {
        Configured,
        Running,
        Released,
    };

    Session(ours::Duration period, ours::SamplerTargets targets);

    /// Allocates the buffers, their slots and, for the kernel target, the
    /// unwind index, and wires them into m_recorder and m_unwinder. NoMemory
    /// if the buffers cannot be had; an index that cannot be built only
    /// costs depth, as kernel records keep their pc alone.
    [[nodiscard]]
    auto allocate(usize records_per_cpu) -> ours::Status;

    TA_REQUIRES(LOCK)
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
    /// interrupts on. Nothing on the sampling path takes it.
    static ours::sync::Mutex LOCK;

    /// Whether a session's memory is allocated, or a create() is building one
    /// under the claim it takes before it allocates. Set by create(); cleared
    /// by the session's Claim when the session is destroyed, or by create()
    /// when no session could be built to hold it.
    static bool CLAIMED TA_GUARDED(LOCK);

    /// A session's hold on CLAIMED, taken by create() before the session is
    /// built and given back when the session is destroyed. The first member,
    /// so it is destroyed last, after the buffers and the unwind index: what
    /// the claim bounds is the memory sessions hold, and a released session
    /// still holds its buffers while a read copies out of them.
    class Claim final {
      public:
        Claim() = default;
        ~Claim();

        Claim(Claim const &) = delete;
        auto operator=(Claim const &) -> Claim & = delete;
    };

    /// The running session; null when none runs. The one gate every writer
    /// passes: the timer's handler writes only while it names the handler's
    /// session, and a user-stack writer records into the session it names.
    /// The recorder has no running state of its own.
    ///
    /// Set by start() after the buffers are emptied and before any timer is
    /// armed; its release publishes the emptied buffers and the generation.
    /// Cleared by stop before its barrier, which is what makes the barrier
    /// cover the writers: each loads it and writes with interrupts off.
    static std::atomic<Session *> ACTIVE;

    /// One sampling timer per CPU, shared by every session in turn.
    /// Allocated by the first start() rather than at boot, so a kernel nobody
    /// profiles never has them, and kept after that: a Timer is a few words.
    static Timers TIMERS TA_GUARDED(LOCK);

    // The first member: see Claim.
    [[no_unique_address]]
    Claim m_claim;

    ours::Duration const m_period;
    ours::SamplerTargets const m_targets;

    /// Which run this is, among every run of every session this boot
    /// starts: what the timer leaves on a thread it marks, and what
    /// sample_user_stack() checks against the running session. Drawn anew by
    /// each start(), not once per session: a stop and a start empty the
    /// buffers, and a mark the old run left on a thread must stay out of
    /// them. A number, not this pointer, since a later session can be
    /// allocated where an earlier one was freed. Written only by start(),
    /// while no timer of this session is armed and ACTIVE does not name
    /// it; the ACTIVE release and the arming IPIs publish it.
    u64 m_generation = 0;

    /// The next run's generation, from 1, so that 0 names none.
    static std::atomic<u64> NEXT_GENERATION;
    State m_state TA_GUARDED(LOCK) = State::Released;

    /// Reads copying out of the buffers now, which start() waits for by
    /// refusing.
    u32 m_readers TA_GUARDED(LOCK) = 0;

    /// The per-CPU buffer headers, and the slots they borrow: one block,
    /// records_per_cpu for each possible CPU in turn.
    ours::mm::CpuLocal::PerCpu<SampleBuffer> m_buffers;
    ktl::Vec<SampleRecord> m_slots;

    /// The kernel's FDEs, sorted by start address, for the unwinder; empty
    /// when the index could not be built.
    ktl::Vec<unwind::FdeIndex::Entry> m_fde_entries;

    /// Where kernel stacks begin, as the unwinder takes them: the arch's
    /// roots, copied here because the unwinder keeps a pointer to them.
    std::array<StackRoot, std::tuple_size_v<decltype(arch::stack_roots())>> m_stack_roots{};

    /// Turns a kernel frame into its stack, from m_fde_entries and
    /// m_stack_roots.
    KernelUnwinder m_unwinder;

    Recorder m_recorder;
};
} // namespace sampler

#endif // #ifndef SAMPLER_SESSION_HPP
