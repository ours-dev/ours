/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prof/sampler.hpp>

#include <arch/tick.hpp>
#include <gktl/command.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/vec.hpp>
#include <logz4/log.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <prof/arch.hpp>
#include <unwind/fde_index.hpp>
#include <unwind/unwinder.hpp>
#include <ustl/sync/atomic.hpp>

#include <prof/format.hpp>

namespace prof {
namespace {
    constexpr std::uint32_t STATE_STOPPED = 0;
    constexpr std::uint32_t STATE_RUNNING = 1;
    /// Set while stopping; PMI handlers may still see it mid-flight.
    constexpr std::uint32_t STATE_STOPPING = 2;

    /// Stack window handed to the unwinder's memory reader: samples only
    /// read slots from the interrupted thread's kernel stack, so a corrupt
    /// frame pointer cannot turn into a wild kernel read.
    constexpr std::uint64_t STACK_WINDOW = 0x1'0000;

    ustl::sync::Atomic<std::uint32_t> g_state{STATE_STOPPED};
    ustl::sync::Atomic<std::uint64_t> g_period{0};
    ustl::sync::Atomic<std::uint32_t> g_source{static_cast<std::uint32_t>(Source::Pmu)};
    ustl::sync::Atomic<std::uint64_t> g_tick_counter{0};

    // Section bounds published by kernel.ld. Empty until the kernel links
    // with unwind tables; the sampler degrades to depth-1 records then.
    extern "C" const std::uint8_t __eh_frame_start[];
    extern "C" const std::uint8_t __eh_frame_end[];

    unwind::Section g_eh_section{nullptr, 0, unwind::Section::Kind::EhFrame};
    unwind::FdeIndex g_fde_index;
    ktl::Vec<unwind::FdeIndex::Entry> g_fde_entries;

    std::size_t g_buffer_offset = mm::CpuLocal::NOWHERE;

    auto buffer_for(ours::CpuNum cpu) -> SampleBuffer * {
        if (g_buffer_offset == mm::CpuLocal::NOWHERE) {
            return nullptr;
        }
        return reinterpret_cast<SampleBuffer *>(mm::CpuLocal::resolve(g_buffer_offset, cpu));
    }

    struct StackWindow final {
        std::uint64_t lo;
        std::uint64_t hi;
    };

    auto kernel_stack_read(void *context, std::uint64_t address, std::uint64_t *out) -> bool {
        auto const *window = static_cast<StackWindow const *>(context);
        if (address < window->lo || window->hi < address + 8) {
            return false;
        }
        *out = *reinterpret_cast<std::uint64_t const *>(address);
        return true;
    }

    auto unwind_context(SampleContext const &context, SampleRecord *record) -> void {
        unwind::Registers regs{
            .pc = context.pc,
            .sp = context.sp,
            .bp = context.bp,
        };
        StackWindow const window{
            .lo = context.sp,
            .hi = context.sp + STACK_WINDOW,
        };
        unwind::MemoryReader const reader{&window, &kernel_stack_read};
        unwind::Unwinder const unwinder(&g_eh_section, 1, &g_fde_index);

        while (record->depth < MAX_STACK_DEPTH) {
            auto const step = unwinder.step(&regs, reader);
            if (step != unwind::StepResult::Ok) {
                // NoFde is the natural leaf stop; anything else is a prefix.
                if (step != unwind::StepResult::NoFde) {
                    record->flags |= RECORD_TRUNCATED;
                }
                break;
            }
            record->ips[record->depth] = regs.pc;
            ++record->depth;
        }
        if (record->depth == MAX_STACK_DEPTH) {
            record->flags |= RECORD_TRUNCATED;
        }
    }

    auto reset_all_buffers() -> void {
        for (ours::CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
            auto *buffer = buffer_for(cpu);
            if (buffer != nullptr) {
                buffer->reset();
            }
        }
    }

    auto prof_start_command(int argc, char **argv) -> int {
        Config config{.source = Source::Pmu, .period = 1'000'000};
        if (argc > 1 && argv[1][0] == 't') {
            config.source = Source::TimerTick;
            config.period = 8; // every 8th scheduler tick
        }
        if (argc > 2) {
            config.period = 0;
            for (char const *p = argv[2]; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9') {
                    log::info("prof: period must be a number");
                    return 1;
                }
                config.period = config.period * 10 + (*p - '0');
            }
        }

        auto const status = start(config);
        if (status != ours::Status::Ok) {
            log::info("prof: start failed: {}", ours::to_string(status));
            return 1;
        }
        if (config.source == Source::Pmu) {
            log::info("prof: sampling cycles every {} events", config.period);
        } else {
            log::info("prof: sampling every {} scheduler ticks", config.period);
        }
        return 0;
    }

    auto prof_stop_command(int, char **) -> int {
        stop();
        log::info("prof: stopped");
        log_summary();
        return 0;
    }

    auto prof_dump_command(int, char **) -> int {
        if (is_running()) {
            log::info("prof: stop the sampler before dumping");
            return 1;
        }
        dump_serial();
        log::info("prof: dump written to COM1");
        return 0;
    }

    GKTL_COMMAND(ProfStart, "prof.start", prof_start_command,
                 "prof.start [pmu|tick] [period]: start the sampling profiler")
    GKTL_COMMAND(ProfStop, "prof.stop", prof_stop_command, "prof.stop: stop the sampling profiler")
    GKTL_COMMAND(ProfDump, "prof.dump", prof_dump_command,
                 "prof.dump: export raw samples as hex lines over COM1")
} // namespace

auto is_running() -> bool {
    return g_state.load(ustl::sync::MemoryOrder::Relaxed) == STATE_RUNNING;
}

auto active_source() -> Source {
    return static_cast<Source>(g_source.load(ustl::sync::MemoryOrder::Relaxed));
}

auto init() -> void {
    g_eh_section = unwind::Section{
        __eh_frame_start,
        static_cast<std::size_t>(__eh_frame_end - __eh_frame_start),
        unwind::Section::Kind::EhFrame,
    };

    if (g_eh_section.size != 0) {
        auto const counting_sink = [](void *context, unwind::FdeIndex::Entry const &) -> bool {
            ++*static_cast<std::size_t *>(context);
            return true;
        };
        std::size_t count = 0;
        unwind::FdeIndex::build(g_eh_section, 0, &count, counting_sink, &count);

        if (count != 0) {
            if (g_fde_entries.reserve(count)) {
                auto const filling_sink = [](void *context, unwind::FdeIndex::Entry const &entry) -> bool {
                    static_cast<ktl::Vec<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
                    return true;
                };
                std::size_t filled = 0;
                unwind::FdeIndex::build(g_eh_section, 0, &g_fde_entries, filling_sink, &filled);

                // The scan emits FDEs in link order, which is nearly — but
                // not exactly — PC order; an insertion sort finishes in
                // ~O(n) on the near-sorted result.
                for (std::size_t i = 1; i < g_fde_entries.size(); ++i) {
                    auto key = g_fde_entries[i];
                    std::size_t j = i;
                    while (j > 0 && (g_fde_entries[j - 1].pc_begin > key.pc_begin)) {
                        g_fde_entries[j] = g_fde_entries[j - 1];
                        --j;
                    }
                    g_fde_entries[j] = key;
                }
                g_fde_index.init(g_fde_entries.data(), g_fde_entries.size());
                log::info("prof: indexed {} unwind frames ({} bytes of .eh_frame)", filled,
                          g_eh_section.size);
            } else {
                log::warn("prof: FDE index allocation failed; unwinding disabled");
            }
        }
    } else {
        log::warn("prof: kernel has no .eh_frame; unwinding disabled");
    }

    g_buffer_offset = mm::CpuLocal::allocate_raw(sizeof(SampleBuffer), alignof(SampleBuffer));
    if (g_buffer_offset == mm::CpuLocal::NOWHERE) {
        log::warn("prof: per-CPU buffer allocation failed; profiling disabled");
        return;
    }
    reset_all_buffers();

    auto const pmu = arch::pmu_present();
    if (pmu) {
        log::info("prof: architectural PMU available");
    } else {
        log::info("prof: no architectural PMU; use 'prof.start tick'");
    }
}

auto start(Config const &config) -> ours::Status {
    auto expected = STATE_STOPPED;
    if (!g_state.compare_exchange_strong(expected, STATE_RUNNING,
                                         ustl::sync::MemoryOrder::AcqRel)) {
        return ours::Status::BadState;
    }

    reset_all_buffers();
    g_period.store(config.period, ustl::sync::MemoryOrder::Relaxed);
    g_tick_counter.store(0, ustl::sync::MemoryOrder::Relaxed);
    g_source.store(static_cast<std::uint32_t>(config.source), ustl::sync::MemoryOrder::Relaxed);

    if (config.source == Source::TimerTick) {
        // Nothing to program: the scheduler tick drives sampling.
        return ours::Status::Ok;
    }

    if (!arch::pmu_present()) {
        g_state.store(STATE_STOPPED, ustl::sync::MemoryOrder::Release);
        return ours::Status::NotSupported;
    }

    auto const status = arch::start_pmu_sampling(config.period);
    if (status != ours::Status::Ok) {
        g_state.store(STATE_STOPPED, ustl::sync::MemoryOrder::Release);
        return status;
    }
    return ours::Status::Ok;
}

auto stop() -> void {
    auto expected = STATE_RUNNING;
    if (!g_state.compare_exchange_strong(expected, STATE_STOPPING,
                                         ustl::sync::MemoryOrder::AcqRel)) {
        return;
    }

    if (active_source() == Source::Pmu) {
        arch::stop_pmu_sampling();
    }
    g_state.store(STATE_STOPPED, ustl::sync::MemoryOrder::Release);
}

auto record_sample(SampleContext const &context) -> void {
    if (g_state.load(ustl::sync::MemoryOrder::Relaxed) != STATE_RUNNING) {
        return;
    }

    auto *buffer = buffer_for(ours::Cpu::id());
    if (buffer == nullptr) {
        return;
    }

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.depth = 1;
    record.cpu = static_cast<std::uint16_t>(ours::Cpu::id());
    record.tid = context.tid;
    record.tsc = arch::Tick::get().tsc;
    record.ips[0] = context.pc;

    if (context.from_uspace) {
        // User stacks need the user address space + user unwind info;
        // out of scope for now.
        record.flags = RECORD_FROM_USPACE;
        buffer->try_produce(record);
        return;
    }

    unwind_context(context, &record);
    buffer->try_produce(record);
}

auto on_scheduler_tick() -> void {
    if (g_state.load(ustl::sync::MemoryOrder::Relaxed) != STATE_RUNNING) {
        return;
    }
    if (active_source() != Source::TimerTick) {
        return;
    }

    std::uint64_t const period = g_period.load(ustl::sync::MemoryOrder::Relaxed);
    if (period == 0) {
        return;
    }
    if ((g_tick_counter.fetch_add(1, ustl::sync::MemoryOrder::Relaxed) % period) != 0) {
        return;
    }

    auto *frame = ours::irq::current_frame();
    if (frame == nullptr) {
        return;
    }

    SampleContext context = {
        .pc = frame->ip,
        .sp = frame->usp,
        .bp = frame->bp,
        .tid = static_cast<std::uint32_t>(ours::task::Thread::Current::tid()),
        .from_uspace = (frame->cs & 0x3) != 0,
    };
    record_sample(context);
}

} // namespace prof
