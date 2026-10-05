/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prof/format.hpp>
#include <prof/sampler.hpp>
#include <prof/sink.hpp>

namespace prof {

auto Sampler::num_cpus() const -> u32 {
    return m_sink.num_cpus();
}

auto Sampler::buffer_stats(u32 cpu) const -> BufferStats {
    return m_sink.stats(cpu);
}

auto Sampler::export_dump(LineWriter const &writer) const -> void {
    auto const totals = m_sink.totals();

    DumpHeader header = {};
    header.magic = DUMP_MAGIC;
    header.version = DUMP_VERSION;
    header.header_size = sizeof(DumpHeader);
    header.record_size = sizeof(SampleRecord);
    header.num_records = static_cast<u32>(totals.retained);
    header.num_dropped = static_cast<u32>(totals.dropped);
    header.load_base = m_image_base;
    header.num_cpus = m_sink.num_cpus();

    write_hex_line<sizeof(DumpHeader)>(writer, DUMP_HEADER_TAG, &header, sizeof(header));

    // A slot that was never filled has no magic; the sink does not know the
    // record's layout, so the decision of what counts as a real record
    // stays here.
    m_sink.export_records(writer, DUMP_RECORD_TAG, [](SampleRecord const &record) noexcept -> bool {
        return record.magic == SAMPLE_MAGIC;
    });
}

} // namespace prof
