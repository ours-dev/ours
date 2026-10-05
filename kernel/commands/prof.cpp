/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `prof.start` / `prof.stop` / `prof.dump`: the kernel shell's way to drive a
// sampling session (ours::sampler::Session).
//
// The session owns everything a run records into, allocated when it is
// created, so these commands hold nothing until the first prof.start. The
// shell holds its session from prof.start until prof.dump gives it back, and
// while it holds one no other session can be created.
#include <ours/sampler/mod.hpp>
#include <ours/status.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <prof/format.hpp>

namespace ours {
namespace {
    /// Records per CPU: at 1 kHz, about a tenth of a second of samples.
    constexpr usize RECORDS_PER_CPU = 128;

    /// The shell's session. Only the console thread runs these commands, one
    /// at a time, so nothing else touches it.
    ustl::Box<sampler::Session> g_session;

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

        // A session's period is fixed when it is created, so each start gets
        // a session of its own; one still held from an earlier start, dumped
        // or not, goes first, and its destructor stops it if it runs.
        if (g_session) {
            log::info("prof: discarding the previous session");
            g_session = nullptr;
        }
        auto const config = sampler::Config{
            .period = Duration(static_cast<Duration::rep>(period_ns)),
            .buffer_size = RECORDS_PER_CPU * sizeof(prof::SampleRecord),
        };
        if (auto const status = sampler::Session::create(config, &g_session);
            status != Status::Ok) {
            log::info("prof: create failed: {}", to_string(status));
            return 1;
        }
        if (auto const status = g_session->start(); status != Status::Ok) {
            log::info("prof: start failed: {}", to_string(status));
            g_session = nullptr;
            return 1;
        }
        log::info("prof: sampling every {} ns", period_ns);
        return 0;
    }

    auto log_summary() -> void {
        u64 total = 0;
        u64 dropped = 0;
        for (u32 cpu = 0; cpu < g_session->num_cpus(); ++cpu) {
            auto const stats = g_session->stats(cpu);
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
        if (!g_session) {
            log::info("prof: no session; prof.start first");
            return 1;
        }
        if (auto const status = g_session->stop(); status != Status::Ok) {
            log::info("prof: stop failed: {}", to_string(status));
            return 1;
        }
        log::info("prof: stopped");
        log_summary();
        return 0;
    }

    auto prof_dump_command(int, char **) -> int {
        if (!g_session) {
            log::info("prof: no session; prof.start first");
            return 1;
        }
        // The logger's sink takes bytes as they are, with no level or
        // timestamp around them, which is what keeps each PROF1/SAMP line
        // parseable; it is the same path the debug_write syscall uses.
        auto const to_console = [](void *, char const *bytes, usize len) -> void {
            log::get_global_logger().write(bytes, len);
        };
        if (auto const status = g_session->export_dump(prof::LineWriter{nullptr, to_console});
            status != Status::Ok) {
            log::info("prof: stop the sampler before dumping");
            return 1;
        }
        // Handed back once dumped, so that a program may create a session.
        g_session = nullptr;
        log::info("prof: dump written to the console");
        return 0;
    }

    GKTL_COMMAND(ProfStart, "prof.start", prof_start_command,
                 "prof.start [period-ns]: start a sampling session")
    GKTL_COMMAND(ProfStop, "prof.stop", prof_stop_command, "prof.stop: stop the sampling session")
    GKTL_COMMAND(ProfDump, "prof.dump", prof_dump_command,
                 "prof.dump: export the session's samples as hex lines, and end it")
} // namespace
} // namespace ours
