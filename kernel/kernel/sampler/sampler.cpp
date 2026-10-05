/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/sampler/mod.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <arch/interrupt.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <unwind/fde_index.hpp>

namespace ours::sampler {
// Section bounds published by kernel.ld, and the runtime load address of the
// kernel image: the ELF is linked at 0 and executed position-independently,
// so the address of the link-script origin symbol is the live base.
// NOLINTBEGIN(readability-identifier-naming) -- linker-script symbols.
extern "C" const u8 __eh_frame_start[];
extern "C" const u8 __eh_frame_end[];
extern "C" const u8 __executable_start[];
// NOLINTEND(readability-identifier-naming)

namespace {
    /// What arm_this_cpu() needs. sync_exec() hands its task one pointer, and
    /// returns only once every CPU has run it, so this can live on the
    /// caller's stack.
    struct ArmRequest final {
        mm::CpuLocal::PerCpu<time::Timer> *timers;
        Session *session;
        Duration period;
    };

    /// The context the timer interrupted, in the terms prof::Sampler takes.
    ///
    /// Everything here is the kernel's own knowledge: the frame the interrupt
    /// preamble recorded, the CPU and the thread it is running, the time. It
    /// is gathered here and handed to the library, rather than the library
    /// asking an arch seam for it, because none of it is a fact about the
    /// architecture -- the frame's accessors are the one arch-specific part,
    /// and they belong to the frame.
    auto interrupted_context(arch::IrqFrame const &frame) -> prof::SampleContext {
        return prof::SampleContext{
            .pc = frame.pc(),
            .sp = frame.sp(),
            .bp = frame.fp(),
            .tid = static_cast<u32>(task::Thread::Current::tid()),
            .cpu = static_cast<u32>(Cpu::id()),
            .timestamp = static_cast<u64>(time::SteadyClock::now().time_since_epoch().count()),
            .from_uspace = frame.from_user(),
        };
    }

    /// The kernel's own `.eh_frame`, as the unwinder reads it.
    auto kernel_eh_frame() -> unwind::Section {
        return unwind::Section{
            __eh_frame_start,
            static_cast<usize>(__eh_frame_end - __eh_frame_start),
            unwind::Section::Kind::EhFrame,
        };
    }

    /// An FDE index under construction: the array and how far it is filled.
    struct FdeTable final {
        unwind::FdeIndex::Entry *entries = nullptr;
        usize capacity = 0;
        usize count = 0;
    };

    /// Builds the index of |section|'s FDEs, sorted by start address, in an
    /// array allocated at its exact size; the caller owns it. Empty, after
    /// saying why, when the section has no FDEs, is malformed, or the array
    /// cannot be had.
    ///
    /// Two passes: the first only counts, so the array never grows. The
    /// section is read-only text, so both passes see the same bytes; the
    /// capacity check is what keeps the second honest if they ever disagree.
    auto build_fde_table(unwind::Section const &section) -> FdeTable {
        using Entry = unwind::FdeIndex::Entry;

        if (section.size == 0) {
            return {};
        }
        auto const counting_sink = [](void *context, Entry const &) -> bool {
            ++*static_cast<usize *>(context);
            return true;
        };
        usize count = 0;
        if (!unwind::FdeIndex::build(section, 0, &count, counting_sink, &count)) {
            log::warn("sampler: malformed .eh_frame; samples will not unwind");
            return {};
        }
        if (count == 0) {
            return {};
        }

        // About 32 bytes an FDE and a few thousand FDEs, which is past the
        // largest kmalloc size class; new routes it to vmalloc, which rounds
        // to a page and no further.
        auto *entries = new (mm::GAF_KERNEL) Entry[count];
        if (entries == nullptr) {
            log::warn("sampler: no memory for the unwind index; samples will not unwind");
            return {};
        }
        auto table = FdeTable{entries, count, 0};

        auto const filling_sink = [](void *context, Entry const &entry) -> bool {
            auto *filling = static_cast<FdeTable *>(context);
            if (filling->count == filling->capacity) {
                return false;
            }
            filling->entries[filling->count] = entry;
            ++filling->count;
            return true;
        };
        usize filled = 0;
        if (!unwind::FdeIndex::build(section, 0, &table, filling_sink, &filled)) {
            log::warn("sampler: .eh_frame changed shape between passes; samples will not unwind");
            delete[] entries;
            return {};
        }

        // The scan emits FDEs in link order, which is nearly -- but not
        // exactly -- address order; an insertion sort finishes in about one
        // pass over the nearly sorted result.
        for (usize i = 1; i < table.count; ++i) {
            auto const key = entries[i];
            usize j = i;
            while (j > 0 && entries[j - 1].pc_begin > key.pc_begin) {
                entries[j] = entries[j - 1];
                --j;
            }
            entries[j] = key;
        }
        return table;
    }

    /// Finds |cpu|'s buffer for prof::Sampler; null for a CPU out of range.
    auto lookup_buffer(void *context, u32 cpu) -> prof::SampleBuffer * {
        if (cpu >= MAX_CPU) {
            return nullptr;
        }
        auto *buffers = static_cast<mm::CpuLocal::PerCpu<prof::SampleBuffer> *>(context);
        return &buffers->ref_mut(cpu);
    }
} // namespace

sync::Mutex Session::s_lock;
bool Session::s_claimed = false;
Session::Timers Session::s_timers;

Session::Session(Duration const period) : m_period(period) {}

auto Session::create(Config const &config, ustl::Box<Session> *const out) -> Status {
    if (config.period < MIN_PERIOD) {
        return Status::InvArgs;
    }
    auto const records_per_cpu = config.buffer_size / sizeof(prof::SampleRecord);
    if (records_per_cpu == 0 || config.buffer_size > MAX_BUFFER_SIZE) {
        return Status::InvArgs;
    }

    Guard<sync::Mutex> guard(s_lock);
    if (s_claimed) {
        return Status::AlreadyExists;
    }

    auto session = ustl::Box<Session>::adopt(new (mm::GAF_KERNEL) Session(config.period));
    if (!session) {
        return Status::NoMemory;
    }
    if (auto const status = session->allocate(records_per_cpu); status != Status::Ok) {
        return status;
    }

    s_claimed = true;
    *out = ustl::move(session);
    return Status::Ok;
}

auto Session::allocate(usize const records_per_cpu) -> Status {
    m_buffers = mm::CpuLocal::allocate<prof::SampleBuffer>(1, alignof(prof::SampleBuffer));
    if (!m_buffers) {
        return Status::NoMemory;
    }

    // One block for every possible CPU's slots. A CPU that is not possible
    // keeps a buffer with none, which drops whatever reaches it.
    auto const cpus = Cpu::possible_mask();
    m_slots = new (mm::GAF_KERNEL) prof::SampleRecord[cpus.count() * records_per_cpu];
    if (m_slots == nullptr) {
        return Status::NoMemory;
    }
    usize next = 0;
    cpus.for_each([this, records_per_cpu, &next](CpuNum const cpu) {
        m_buffers.ref_mut(cpu).attach(m_slots + next * records_per_cpu, records_per_cpu);
        ++next;
    });

    auto const eh_frame = kernel_eh_frame();
    auto const fdes = build_fde_table(eh_frame);
    m_fde_entries = fdes.entries;

    auto const flags = m_sampler.init({
        .buffer_lookup = &lookup_buffer,
        .buffer_lookup_context = &m_buffers,
        .num_cpus = static_cast<u32>(MAX_CPU),
        .eh_section = eh_frame,
        .fde_entries = fdes.entries,
        .fde_entry_count = fdes.count,
        .image_base = reinterpret_cast<u64>(__executable_start),
    });
    if (!prof::has_flag(flags, prof::Sampler::InitFlags::BuffersReady)) {
        return Status::NoMemory;
    }
    return Status::Ok;
}

Session::~Session() {
    release();
    // A reader holds a reference to whatever owns this session, so none can
    // be counted in once the owner lets go.
    DEBUG_ASSERT(m_readers == 0);
    // Nothing can reach the memory now: the timers are disarmed, and the
    // owner holding the Box is the last reference.
    delete[] m_fde_entries;
    delete[] m_slots;
}

auto Session::start() -> Status {
    Guard<sync::Mutex> guard(s_lock);
    // Starting empties the buffers, which a read in flight is copying from.
    if (m_state != State::Configured || m_readers != 0) {
        return Status::BadState;
    }
    if (!s_timers) {
        s_timers = mm::CpuLocal::allocate<time::Timer>(1, alignof(time::Timer));
        if (!s_timers) {
            return Status::NoMemory;
        }
    }

    // Running first, timers second: a timer that fires the moment it is
    // armed must already find the session running, or its first sample is
    // lost and, worse, it declines to re-arm. Starting the sampler also
    // empties the buffers of the run before.
    if (auto const status = m_sampler.start(); status != Status::Ok) {
        return status;
    }
    m_state = State::Running;

    auto request = ArmRequest{&s_timers, this, m_period};
    IpiService::sync_exec(Cpu::online_mask(), &arm_this_cpu, &request);
    return Status::Ok;
}

auto Session::stop() -> Status {
    Guard<sync::Mutex> guard(s_lock);
    if (m_state != State::Running) {
        return Status::BadState;
    }
    stop_locked();
    return Status::Ok;
}

/// The cancel is also the guarantee stop() makes to the reader.
///
/// A handler that loaded Running just before Sampler::stop() stored Stopped
/// may still be writing its sample. It runs with interrupts off, so the
/// cancel that sync_exec() sends its CPU is taken only once that handler has
/// returned; and sync_exec() returns only once every CPU has taken it. After
/// that, every handler still to run loads Stopped. So no CPU is mid-write
/// when this returns, which is what lets the buffers be read -- and, for a
/// released session, freed.
///
/// The barrier holds only because every writer checks the state and writes
/// with interrupts off. A writer in thread context, as the user-stack target
/// will be, keeps that only if its check and its write sit in one
/// interrupts-off section; otherwise it needs another way to be waited for,
/// such as a count of writers in flight that stop() sleeps on until it
/// drains.
auto Session::stop_locked() -> void {
    m_sampler.stop();
    IpiService::sync_exec(Cpu::online_mask(), &disarm_this_cpu, &s_timers);
    m_state = State::Configured;
}

auto Session::release() -> void {
    Guard<sync::Mutex> guard(s_lock);
    if (m_state == State::Released) {
        return;
    }
    if (m_state == State::Running) {
        stop_locked();
    }
    m_state = State::Released;
    s_claimed = false;
}

auto Session::export_dump(prof::LineWriter const &writer) -> Status {
    Guard<sync::Mutex> guard(s_lock);
    if (m_state == State::Running) {
        return Status::BadState;
    }
    m_sampler.export_dump(writer);
    return Status::Ok;
}

auto Session::dump_size(usize *const out) -> Status {
    Guard<sync::Mutex> guard(s_lock);
    if (m_state != State::Configured) {
        return Status::BadState;
    }
    *out = m_sampler.dump_size();
    return Status::Ok;
}

auto Session::read(prof::Sampler::ByteWriter const &writer, usize const capacity,
                   usize *const actual) -> Status {
    {
        Guard<sync::Mutex> guard(s_lock);
        if (m_state != State::Configured) {
            return Status::BadState;
        }
        auto const needed = m_sampler.dump_size();
        if (capacity < needed) {
            *actual = needed;
            return Status::BufferTooSmall;
        }
        ++m_readers;
    }

    // Counted in, so the buffers hold still: start() refuses until this read
    // is counted out, and nothing else writes them while the session is not
    // Running. A release meanwhile does not touch them either.
    struct Progress final {
        prof::Sampler::ByteWriter const *inner;
        usize written;
    };
    auto progress = Progress{&writer, 0};
    auto const counting = [](void *context, usize offset, void const *bytes, usize len) -> Status {
        auto *state = static_cast<Progress *>(context);
        auto const status = state->inner->write(state->inner->context, offset, bytes, len);
        if (status == Status::Ok) {
            state->written = offset + len;
        }
        return status;
    };
    auto const status = m_sampler.copy_dump({&progress, counting});
    *actual = progress.written;

    Guard<sync::Mutex> guard(s_lock);
    --m_readers;
    return status;
}

auto Session::stats(u32 const cpu) -> prof::BufferStats {
    Guard<sync::Mutex> guard(s_lock);
    return m_sampler.buffer_stats(cpu);
}

auto Session::num_cpus() const -> u32 {
    return m_sampler.num_cpus();
}

/// It goes quiet as soon as the session stops rather than waiting for stop()
/// to cancel it, which is the same answer either way: a sample taken after
/// the state left Running would be refused by record_sample() anyway, and
/// not re-arming saves the CPU one more pointless interrupt before the
/// cancel lands.
///
/// |self| outlives every expiry: the session is destroyed only after it is
/// released, and releasing a running session stops it, which disarms this
/// timer on its own CPU before returning.
auto Session::on_sample_due(Session *const self, Duration const period, time::Timer *const timer)
    -> time::Timer::HandlerResult {
    auto &sampler = self->m_sampler;
    if (!sampler.is_running()) {
        return time::Timer::HandlerResult::Handled;
    }

    // The timer interrupt took the interrupt preamble like any other
    // vector, so the frame it interrupted is this CPU's current frame. Only
    // kernel frames are sampled: walking a user stack needs the user target,
    // which samples on the way back to user mode instead.
    if (auto const *frame = irq::current_frame(); frame != nullptr && !frame->from_user()) {
        sampler.record_sample(interrupted_context(*frame));
    }

    timer->forward(period);
    return time::Timer::HandlerResult::Restart;
}

/// Timer::activate() queues on the calling CPU's TimerQueue, so running this
/// on each CPU is what puts one timer on each.
auto Session::arm_this_cpu(void *const context) -> void {
    auto const *request = static_cast<ArmRequest const *>(context);
    auto const first = time::SteadyClock::now() + request->period;
    request->timers->ref_mut().activate(time::Deadline::at(first), &on_sample_due, request->session,
                                        request->period);
}

/// Run here, on the timer's own CPU with interrupts off, the cancel cannot
/// overlap the handler: the handler runs only in this CPU's timer interrupt,
/// which cannot start while this runs. So the cancel never has to wait, and
/// whether it found the timer queued or the handler had already declined to
/// re-arm, the timer is off once it returns.
auto Session::disarm_this_cpu(void *const context) -> void {
    auto *timers = static_cast<Timers *>(context);
    // Which of the two it was does not matter; see above.
    auto _ = timers->ref_mut().cancel();
}
} // namespace ours::sampler
