/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sampler/format.hpp>
#include <sampler/recorder.hpp>
#include <sampler/sink.hpp>

namespace sampler {

auto Recorder::num_cpus() const -> u32 {
    return m_sink.num_cpus();
}

auto Recorder::buffer_stats(u32 cpu) const -> BufferStats {
    return m_sink.stats(cpu);
}

auto Recorder::make_header() const -> DumpHeader {
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
    header.num_unmatched = totals.skipped;
    return header;
}

auto Recorder::export_dump(LineWriter const &writer) const -> void {
    auto const header = make_header();
    write_hex_line<sizeof(DumpHeader)>(writer, DUMP_HEADER_TAG, &header, sizeof(header));

    // A slot that was never filled has no magic; the sink does not know the
    // record's layout, so the decision of what counts as a real record
    // stays here.
    m_sink.export_records(writer, DUMP_RECORD_TAG, [](SampleRecord const &record) noexcept -> bool {
        return record.magic == SAMPLE_MAGIC;
    });
}

auto Recorder::dump_size() const -> usize {
    return sizeof(DumpHeader) + m_sink.totals().retained * sizeof(SampleRecord);
}

auto Recorder::copy_dump(ByteWriter const &writer) const -> ours::Status {
    auto const header = make_header();
    auto status = writer.write(writer.context, 0, &header, sizeof(header));

    // Every retained record goes, filled or not, so that the bytes written
    // are exactly what dump_size() promised and the header's record count
    // describes them. Once sampling has stopped every retained slot has been
    // written, so none is unfilled in practice; a reader still checks each
    // record's magic, as prof-fold does.
    auto offset = sizeof(DumpHeader);
    for (u32 cpu = 0; cpu < m_sink.num_cpus() && status == ours::Status::Ok; ++cpu) {
        auto const *buffer = m_sink.buffer_for(cpu);
        if (buffer == nullptr) {
            continue;
        }
        buffer->for_each_retained([&](SampleRecord const &record) {
            if (status != ours::Status::Ok) {
                return;
            }
            status = writer.write(writer.context, offset, &record, sizeof(record));
            offset += sizeof(record);
        });
    }
    return status;
}

} // namespace sampler
