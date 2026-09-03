/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prof/arch.hpp>
#include <prof/format.hpp>
#include <prof/sampler.hpp>
#include <unwind/unwinder.hpp>

#include <ours/status.hpp>

namespace prof {
namespace {
    /// Stack window handed to the unwinder's memory reader: samples only
    /// read slots from the interrupted thread's kernel stack, so a corrupt
    /// frame pointer cannot turn into a wild kernel read.
    constexpr u64 STACK_WINDOW = 0x1'0000;

    struct StackWindow final {
        u64 lo;
        u64 hi;
    };

    auto kernel_stack_read(void *context, u64 address, u64 *out) -> bool {
        auto const *window = static_cast<StackWindow const *>(context);
        if (address < window->lo || window->hi < address + 8) {
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

auto Sampler::buffer_for(u32 cpu) const -> SampleBuffer * {
    if (m_buffer_lookup == nullptr) {
        return nullptr;
    }
    return m_buffer_lookup(m_buffer_lookup_context, cpu);
}

auto Sampler::reset_all_buffers() -> void {
    for (u32 cpu = 0; cpu < m_num_cpus; ++cpu) {
        auto *buffer = buffer_for(cpu);
        if (buffer != nullptr) {
            buffer->reset();
        }
    }
}

auto Sampler::unwind_context(SampleContext const &context, SampleRecord *record) const -> void {
    unwind::Registers regs{
        .pc = context.pc,
        .sp = context.sp,
        .bp = context.bp,
    };
    StackWindow window{
        .lo = context.sp,
        .hi = context.sp + STACK_WINDOW,
    };
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
    m_buffer_lookup = resources.buffer_lookup;
    m_buffer_lookup_context = resources.buffer_lookup_context;
    m_num_cpus = resources.num_cpus;

    m_eh_section = resources.eh_section;
    m_fde_index.init(resources.fde_entries, resources.fde_entry_count);
    m_image_base = resources.image_base;

    reset_all_buffers();

    // A non-null lookup only proves the caller wired one in, not that it
    // actually resolves — probe CPU 0 so `BuffersReady` reflects reality
    // even when the callback itself degrades to "no storage" internally
    // (e.g. the kernel's own CpuLocal allocation having failed).
    auto flags = InitFlags::None;
    if (m_num_cpus != 0 && buffer_for(0) != nullptr) {
        flags = flags | InitFlags::BuffersReady;
    }
    if (resources.fde_entry_count != 0) {
        flags = flags | InitFlags::UnwindReady;
    }
    return flags;
}

auto Sampler::is_running() const -> bool {
    return m_state.load(std::memory_order_relaxed) == State::Running;
}

auto Sampler::active_source() const -> Source {
    return m_source.load(std::memory_order_relaxed);
}

auto Sampler::active_period() const -> u64 {
    return m_period.load(std::memory_order_relaxed);
}

auto Sampler::start(Config const &config) -> ours::Status {
    auto expected = State::Stopped;
    if (!m_state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel)) {
        return ours::Status::BadState;
    }

    reset_all_buffers();
    m_period.store(config.period, std::memory_order_relaxed);
    m_tick_counter.store(0, std::memory_order_relaxed);
    m_source.store(config.source, std::memory_order_relaxed);

    if (config.source == Source::TimerTick) {
        // Nothing to program: the scheduler tick drives sampling.
        return ours::Status::Ok;
    }

    if (!arch::pmu_present()) {
        m_state.store(State::Stopped, std::memory_order_release);
        return ours::Status::NotSupported;
    }

    auto const status = arch::start_pmu_sampling(config.period);
    if (status != ours::Status::Ok) {
        m_state.store(State::Stopped, std::memory_order_release);
        return status;
    }
    return ours::Status::Ok;
}

auto Sampler::stop() -> void {
    auto expected = State::Running;
    if (!m_state.compare_exchange_strong(expected, State::Stopping, std::memory_order_acq_rel)) {
        return;
    }

    if (active_source() == Source::Pmu) {
        arch::stop_pmu_sampling();
    }
    m_state.store(State::Stopped, std::memory_order_release);
}

auto Sampler::record_sample(SampleContext const &context) -> void {
    if (m_state.load(std::memory_order_relaxed) != State::Running) {
        return;
    }

    auto const cpu = arch::current_cpu();
    auto *buffer = buffer_for(cpu);
    if (buffer == nullptr) {
        return;
    }

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
        buffer->try_produce(record);
        return;
    }

    unwind_context(context, &record);
    buffer->try_produce(record);
}

auto Sampler::on_scheduler_tick() -> void {
    if (m_state.load(std::memory_order_relaxed) != State::Running) {
        return;
    }
    if (active_source() != Source::TimerTick) {
        return;
    }

    u64 const period = m_period.load(std::memory_order_relaxed);
    if (period == 0) {
        return;
    }
    if ((m_tick_counter.fetch_add(1, std::memory_order_relaxed) % period) != 0) {
        return;
    }

    auto const context = arch::capture_sample_context();
    if (!context.has_value()) {
        return;
    }
    record_sample(*context);
}

} // namespace prof
