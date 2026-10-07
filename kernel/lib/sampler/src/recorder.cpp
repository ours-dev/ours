/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sampler/format.hpp>
#include <sampler/recorder.hpp>

#include <algorithm>

namespace sampler {

auto Recorder::init(Resources const &resources) -> bool {
    // `Sink::init` reports whether a buffer actually resolves, not merely
    // whether a lookup was wired in: the kernel's own CpuLocal allocation
    // can have failed, leaving a valid callback that yields nothing.
    return m_sink.init(Sink::Resources{
        .buffer_lookup = resources.buffer_lookup,
        .buffer_lookup_context = resources.buffer_lookup_context,
        .num_cpus = resources.num_cpus,
    });
}

auto Recorder::reset() -> void {
    m_sink.reset();
}

auto Recorder::record(SampleContext const &context, std::span<u64 const> const ips,
                      RecordFlags const flags) -> void {
    if (ips.empty()) {
        return;
    }

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.flags = flags;
    record.cpu = static_cast<u16>(context.cpu);
    record.tid = context.tid;
    record.pid = context.pid;
    record.timestamp = context.timestamp;
    auto const kept = std::min(ips.size(), MAX_STACK_DEPTH);
    std::ranges::copy(ips.first(kept), record.ips);
    record.depth = static_cast<u8>(kept);
    auto _ = m_sink.produce(context.cpu, record);
}

auto Recorder::note_unmatched(u32 const cpu) -> void {
    m_sink.skip(cpu);
}

} // namespace sampler
