/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prof/arch.hpp>
#include <prof/format.hpp>
#include <prof/sampler.hpp>

#include <atomic>

#include <ours/status.hpp>
#include <unwind/unwinder.hpp>

namespace prof {
namespace {
    /// Stack window handed to the unwinder's memory reader: samples only
    /// read slots from the interrupted thread's kernel stack, so a corrupt
    /// frame pointer cannot turn into a wild kernel read.
    constexpr u64 STACK_WINDOW = 0x1'0000;

    static_assert(STACK_WINDOW >= sizeof(u64),
                  "the window must hold at least one slot, or hi - sizeof(u64) underflows");

    struct StackWindow final {
        u64 lo;
        u64 hi;

        /// Builds the window above `sp`, saturating at the top of the address
        /// space.
        ///
        /// `sp + STACK_WINDOW` is not safe to write plainly: a stack pointer
        /// near the top would wrap the end to a low address, and every bounds
        /// check against it afterwards would pass. `sp` comes out of an
        /// interrupted CPU's trap frame, so it is exactly the kind of value
        /// this window exists to distrust.
        [[nodiscard]]
        static auto above(u64 sp) -> StackWindow {
            u64 const room = ~u64{0} - sp;
            return StackWindow{
                .lo = sp,
                .hi = room < STACK_WINDOW ? ~u64{0} : sp + STACK_WINDOW,
            };
        }
    };

    /// Reads one word of the interrupted thread's stack, refusing anything
    /// outside the window.
    ///
    /// The comparison is deliberately written as `address > hi - sizeof(u64)`
    /// rather than `hi < address + sizeof(u64)`. `address` is an unwound frame
    /// pointer, i.e. the one input here that may be arbitrary: near the top of
    /// the address space `address + 8` wraps to a small number, `hi` compares
    /// greater, and the check the comment above this window promised -- that a
    /// corrupt frame pointer cannot become a wild kernel read -- silently
    /// stopped holding. `hi - sizeof(u64)` cannot underflow because the window
    /// is at least that wide (static_assert above) and `above()` never wraps.
    ///
    /// Alignment is checked for the same reason. x86 tolerates an unaligned
    /// load, but this library is built for arm and riscv too, where the same
    /// corrupt pointer would fault inside an interrupt handler instead of
    /// being rejected.
    auto kernel_stack_read(void *context, u64 address, u64 *out) -> bool {
        auto const *window = static_cast<StackWindow const *>(context);
        if (address < window->lo || address > window->hi - sizeof(u64)) {
            return false;
        }
        if ((address % alignof(u64)) != 0) {
            return false;
        }
        *out = *reinterpret_cast<u64 const *>(address);
        return true;
    }
} // namespace

auto Sampler::get() -> Sampler & {
    static Sampler instance;
    return instance;
}

auto Sampler::unwind_context(SampleContext const &context, SampleRecord *record) const -> void {
    unwind::Registers regs{
        .pc = context.pc,
        .sp = context.sp,
        .bp = context.bp,
    };
    auto window = StackWindow::above(context.sp);
    unwind::MemoryReader const reader{&window, &kernel_stack_read};
    unwind::Unwinder const unwinder(&m_eh_section, 1, &m_fde_index);

    while (record->depth < MAX_STACK_DEPTH) {
        auto const step = unwinder.step(&regs, reader);
        if (step != unwind::StepResult::Ok) {
            // NoFde is the natural leaf stop; anything else is a prefix.
            if (step != unwind::StepResult::NoFde) {
                record->flags |= RecordFlags::RecordTruncated;
            }
            break;
        }
        record->ips[record->depth] = regs.pc;
        ++record->depth;
    }
    if (record->depth == MAX_STACK_DEPTH) {
        record->flags |= RecordFlags::RecordTruncated;
    }
}

auto Sampler::init(Resources const &resources) -> InitFlags {
    auto const buffers_ready = m_sink.init(Sink::Resources{
        .buffer_lookup = resources.buffer_lookup,
        .buffer_lookup_context = resources.buffer_lookup_context,
        .num_cpus = resources.num_cpus,
    });

    m_eh_section = resources.eh_section;
    m_fde_index.init(resources.fde_entries, resources.fde_entry_count);
    m_image_base = resources.image_base;

    // `Sink::init` reports whether a buffer actually resolves, not merely
    // whether a lookup was wired in: the kernel's own CpuLocal allocation
    // can have failed, leaving a valid callback that yields nothing.
    auto flags = InitFlags::None;
    if (buffers_ready) {
        flags = flags | InitFlags::BuffersReady;
    }
    if (resources.fde_entry_count != 0) {
        flags = flags | InitFlags::UnwindReady;
    }
    return flags;
}

auto Sampler::is_running() const -> bool {
    return m_state.load(std::memory_order_acquire) == State::Running;
}

// The acquire load in the two accessors below is for ordering, not for its
// value: it pairs with the release that published Running after the field
// was written, which is what makes the plain read after it well-defined.
auto Sampler::active_source() const -> Source {
    auto _ = m_state.load(std::memory_order_acquire);
    return m_source;
}

auto Sampler::active_period() const -> u64 {
    auto _ = m_state.load(std::memory_order_acquire);
    return m_period;
}

auto Sampler::start(Config const &config) -> ours::Status {
    // Claim the sampler first, in a state no reader acts on. That is what
    // makes the rest of this function safe to do in the open: the plain
    // configuration fields can be written without anyone observing them
    // half-set, because `Starting` is not `Running`.
    auto expected = State::Stopped;
    if (!m_state.compare_exchange_strong(expected, State::Starting, std::memory_order_acq_rel)) {
        return ours::Status::BadState;
    }

    m_source = config.source;
    m_period = config.period;
    m_sink.reset();
    // Relaxed on purpose. This reset is not a publication point; it is one
    // more write that the release store of Running below publishes, exactly
    // like the two plain assignments above it (a release orders *every*
    // preceding write, atomic or not, before the acquire that reads it). A
    // release here would claim an ordering role this store does not have.
    m_tick_counter.store(0, std::memory_order_relaxed);

    auto const stop_again = [&]() -> void {
        m_state.store(State::Stopped, std::memory_order_release);
    };

    if (config.source == Source::Pmu) {
        if (!arch::pmu_present()) {
            stop_again();
            return ours::Status::NotSupported;
        }
        auto const status = arch::start_pmu_sampling(config.period);
        if (status != ours::Status::Ok) {
            stop_again();
            return status;
        }
    }

    // The release is the publication point: source, period and the reset
    // buffers all happen-before any reader that acquires Running.
    m_state.store(State::Running, std::memory_order_release);
    return ours::Status::Ok;
}

auto Sampler::stop() -> void {
    auto expected = State::Running;
    if (!m_state.compare_exchange_strong(expected, State::Stopping, std::memory_order_acq_rel)) {
        return;
    }

    if (m_source == Source::Pmu) {
        arch::stop_pmu_sampling();
    }
    m_state.store(State::Stopped, std::memory_order_release);
}

auto Sampler::record_sample(SampleContext const &context) -> void {
    if (m_state.load(std::memory_order_acquire) != State::Running) {
        return;
    }

    auto const cpu = arch::current_cpu();

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.depth = 1;
    record.cpu = static_cast<u16>(cpu);
    record.tid = context.tid;
    record.tsc = arch::timestamp();
    record.ips[0] = context.pc;

    if (context.from_uspace) {
        // User stacks need the user address space + user unwind info;
        // out of scope for now.
        record.flags = RecordFlags::RecordFromUspace;
        auto _ = m_sink.produce(cpu, record);
        return;
    }

    unwind_context(context, &record);
    auto _ = m_sink.produce(cpu, record);
}

auto Sampler::on_scheduler_tick() -> void {
    // Acquire first, fields after: see the note on `m_state` in the header
    // for why the two plain reads below are ordered by this load.
    if (m_state.load(std::memory_order_acquire) != State::Running) {
        return;
    }
    if (m_source != Source::TimerTick) {
        return;
    }

    u64 const period = m_period;
    if (period == 0) {
        return;
    }
    if ((m_tick_counter.fetch_add(1, std::memory_order_relaxed) % period) != 0) {
        return;
    }

    auto const context = arch::capture_sample_context();
    if (context.is_none()) {
        return;
    }
    record_sample(*context);
}

} // namespace prof
