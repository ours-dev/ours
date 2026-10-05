/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `prof.start` / `prof.stop` / `prof.dump`, and the kernel-side state the
// standalone `kernel/lib/prof` sampler needs to answer them: the per-CPU
// sample buffers and an unwind FDE index built from the kernel's own
// `.eh_frame`.
//
// None of it lives inside `kernel/lib/prof`, so that library stays a true
// standalone lib — see its README. It does not live under `kernel/kernel`
// either: nothing but these three commands ever touches the buffers or the
// index, so a kernel built without a console has no reason to carry them.
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/sampler/mod.hpp>
#include <ours/status.hpp>

#include <gktl/command.hpp>
#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <prof/sampler.hpp>
#include <unwind/fde_index.hpp>

namespace ours {
namespace {
    /// Records per CPU: at 1 kHz, about a tenth of a second of samples.
    constexpr usize RECORDS_PER_CPU = 128;

    mm::CpuLocal::PerCpu<prof::SampleBuffer> g_buffer;

    // The FDE index array. It is sized by the kernel's own `.eh_frame`
    // (32 bytes per FDE, ~800 FDEs in the dbg image, so ~25 KiB), which is
    // past the largest kmalloc size class.
    //
    // That used to mean it could not use `new` at all: an oversized request
    // answered nullptr, and `ktl::Vec`'s allocator then constructed elements
    // at that nullptr, which is how this hook once hung every boot. It came
    // straight from the frame allocator instead. Both halves of that are
    // fixed now -- `new` routes past the ladder to vmalloc, and the placement
    // forms are noexcept so a failure is a null pointer rather than a
    // constructor running on one -- so this is an ordinary array again.
    //
    // Still a plain array rather than a `ktl::Vec`: the index is built in two
    // passes, the first of which counts, so it is allocated at its exact
    // final size and never grows. It is never freed either; it lives as long
    // as the text it describes.
    struct FdeTable final {
        unwind::FdeIndex::Entry *entries = nullptr;
        usize capacity = 0;
        usize count = 0;
    };
    FdeTable g_fde_table;

    auto lookup_buffer(void *, u32 cpu) -> prof::SampleBuffer * {
        if (!g_buffer) {
            return nullptr;
        }
        return &g_buffer.ref_mut(cpu);
    }

    // Section bounds published by kernel.ld. Empty until the kernel links
    // with unwind tables; the sampler degrades to depth-1 records then.
    // NOLINTNEXTLINE(readability-identifier-naming) -- linker-script symbol.
    extern "C" const u8 __eh_frame_start[];
    // NOLINTNEXTLINE(readability-identifier-naming) -- linker-script symbol.
    extern "C" const u8 __eh_frame_end[];
    // Runtime load address of the kernel image: the ELF is linked at 0 and
    // executed position-independently, so taking the address of the
    // link-script origin symbol yields the live base.
    // NOLINTNEXTLINE(readability-identifier-naming) -- linker-script symbol.
    extern "C" const u8 __executable_start[];

    auto build_fde_index(unwind::Section const &section) -> void {
        using Entry = unwind::FdeIndex::Entry;

        // Two passes: the first only counts, so the table is allocated at
        // its exact final size and the second pass can never outgrow it.
        // The section is read-only text, so both passes see the same bytes.
        auto const counting_sink = [](void *context, Entry const &) -> bool {
            ++*static_cast<usize *>(context);
            return true;
        };
        usize count = 0;
        if (!unwind::FdeIndex::build(section, 0, &count, counting_sink, &count)) {
            log::warn("prof: malformed .eh_frame; unwinding disabled");
            return;
        }
        if (count == 0) {
            return;
        }

        // Exactly `count` entries. The frame allocator this used to call
        // rounds a page count up to a power of two, so ~25 KiB became 32 KiB
        // and the slack was simply lost; vmalloc rounds to a page and stops.
        auto *entries = new (mm::GAF_KERNEL) Entry[count];
        if (entries == nullptr) {
            log::warn("prof: FDE index allocation failed; unwinding disabled");
            return;
        }
        g_fde_table = FdeTable{entries, count, 0};

        // The capacity check is what keeps the sink honest if the two passes
        // ever disagree; the build reports the failure and the partial table
        // is discarded rather than trusted.
        auto const filling_sink = [](void *context, Entry const &entry) -> bool {
            auto *table = static_cast<FdeTable *>(context);
            if (table->count == table->capacity) {
                return false;
            }
            table->entries[table->count] = entry;
            ++table->count;
            return true;
        };
        usize filled = 0;
        if (!unwind::FdeIndex::build(section, 0, &g_fde_table, filling_sink, &filled)) {
            log::warn("prof: .eh_frame changed shape between passes; unwinding disabled");
            g_fde_table = FdeTable{};
            return;
        }

        // The scan emits FDEs in link order, which is nearly -- but not
        // exactly -- PC order; an insertion sort finishes in ~O(n) on the
        // near-sorted result.
        Entry *const sorted = g_fde_table.entries;
        for (usize i = 1; i < g_fde_table.count; ++i) {
            auto key = sorted[i];
            usize j = i;
            while (j > 0 && (sorted[j - 1].pc_begin > key.pc_begin)) {
                sorted[j] = sorted[j - 1];
                --j;
            }
            sorted[j] = key;
        }
        log::info("prof: indexed {} unwind frames ({} bytes of .eh_frame)", filled, section.size);
    }

    // Gives every possible CPU's buffer RECORDS_PER_CPU slots, cut from one
    // allocation that lives as long as the buffers do. A CPU that is not
    // possible keeps a buffer with no slots, which drops whatever reaches it.
    auto attach_slots() -> void {
        if (!g_buffer) {
            return;
        }
        auto const cpus = Cpu::possible_mask();
        auto *slots = new (mm::GAF_KERNEL) prof::SampleRecord[cpus.count() * RECORDS_PER_CPU];
        if (slots == nullptr) {
            log::warn("prof: sample slot allocation failed; profiling disabled");
            return;
        }
        usize next = 0;
        cpus.for_each([slots, &next](CpuNum const cpu) {
            g_buffer.ref_mut(cpu).attach(slots + next * RECORDS_PER_CPU, RECORDS_PER_CPU);
            ++next;
        });
    }

    // Indexing needs the heap and the final image layout; buffers need the
    // per-CPU allocator. Platform level is past both.
    auto prof_init() -> void {
        unwind::Section const eh_section{
            __eh_frame_start,
            static_cast<usize>(__eh_frame_end - __eh_frame_start),
            unwind::Section::Kind::EhFrame,
        };

        if (eh_section.size != 0) {
            build_fde_index(eh_section);
        }

        g_buffer = mm::CpuLocal::allocate<prof::SampleBuffer>(1, alignof(prof::SampleBuffer));
        attach_slots();

        auto const flags = prof::Sampler::get().init({
            .buffer_lookup = &lookup_buffer,
            .buffer_lookup_context = nullptr,
            .num_cpus = static_cast<u32>(MAX_CPU),
            .eh_section = eh_section,
            .fde_entries = g_fde_table.entries,
            .fde_entry_count = g_fde_table.count,
            .image_base = reinterpret_cast<u64>(__executable_start),
        });

        if (!prof::has_flag(flags, prof::Sampler::InitFlags::BuffersReady)) {
            log::warn("prof: per-CPU buffer allocation failed; profiling disabled");
        }
        if (!prof::has_flag(flags, prof::Sampler::InitFlags::UnwindReady)) {
            log::warn("prof: no usable .eh_frame; unwinding disabled");
        }
    }

    auto prof_start_command(int argc, char **argv) -> int {
        u64 period_ns = 1'000'000; // 1 kHz on every CPU
        if (argc > 1) {
            period_ns = 0;
            for (char const *p = argv[1]; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9') {
                    log::info("prof: period must be a number of nanoseconds");
                    return 1;
                }
                period_ns = period_ns * 10 + static_cast<u64>(*p - '0');
            }
        }

        auto const status = sampler::start(Duration(static_cast<Duration::rep>(period_ns)));
        if (status != Status::Ok) {
            log::info("prof: start failed: {}", to_string(status));
            return 1;
        }
        log::info("prof: sampling every {} ns", period_ns);
        return 0;
    }

    auto log_summary() -> void {
        auto &sampler = prof::Sampler::get();
        u64 total = 0;
        u64 dropped = 0;
        for (u32 cpu = 0; cpu < sampler.num_cpus(); ++cpu) {
            auto const stats = sampler.buffer_stats(cpu);
            if (stats.total != 0 || stats.dropped != 0) {
                log::info("prof: cpu{}: {} samples retained, {} dropped ({} total)", cpu,
                          stats.retained, stats.dropped, stats.total);
            }
            total += stats.total;
            dropped += stats.dropped;
        }
        if (total == 0) {
            log::info("prof: no samples collected");
            return;
        }
        log::info("prof: {} samples total, {} dropped", total, dropped);
    }

    auto prof_stop_command(int, char **) -> int {
        sampler::stop();
        log::info("prof: stopped");
        log_summary();
        return 0;
    }

    auto prof_dump_command(int, char **) -> int {
        if (prof::Sampler::get().is_running()) {
            log::info("prof: stop the sampler before dumping");
            return 1;
        }
        // The logger's sink takes bytes as they are, with no level or
        // timestamp around them, which is what keeps each PROF1/SAMP line
        // parseable; it is the same path the debug_write syscall uses.
        auto const to_console = [](void *, char const *bytes, usize len) -> void {
            log::get_global_logger().write(bytes, len);
        };
        prof::Sampler::get().export_dump(prof::LineWriter{nullptr, to_console});
        log::info("prof: dump written to the console");
        return 0;
    }

    GKTL_COMMAND(ProfStart, "prof.start", prof_start_command,
                 "prof.start [period-ns]: start the sampling profiler")
    GKTL_COMMAND(ProfStop, "prof.stop", prof_stop_command, "prof.stop: stop the sampling profiler")
    GKTL_COMMAND(ProfDump, "prof.dump", prof_dump_command,
                 "prof.dump: export raw samples as hex lines on the console")

    GKTL_INIT_HOOK(ProfInit, prof_init, gktl::InitLevel::Platform)
} // namespace
} // namespace ours
