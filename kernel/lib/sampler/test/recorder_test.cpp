/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side coverage for sampler::Recorder's own logic (buffer selection,
// dump encoding) with no kernel underneath it. Every fact a sample needs
// arrives in its SampleContext and its stack as addresses, and every dump
// line leaves through a LineWriter, so there is nothing to fake: linking this
// executable at all proves Recorder needs nothing from
// ours::/gktl::/logz4::/CpuLocal or from an architecture.
#include <sampler/format.hpp>
#include <sampler/recorder.hpp>

#include <array>
#include <string>

#include <cstring>

#include <gtest/gtest.h>
#include <ours/status.hpp>

namespace {

/// Two CPUs' worth of buffers, four records deep: deep enough for every test
/// but the one that fills a buffer on purpose.
constexpr usize DEPTH = 4;
std::array<std::array<sampler::SampleRecord, DEPTH>, 2> SLOTS;
std::array<sampler::SampleBuffer, 2> BUFFERS;

auto lookup(void *, u32 cpu) -> sampler::SampleBuffer * {
    if (cpu >= BUFFERS.size()) {
        return nullptr;
    }
    return &BUFFERS[cpu];
}

/// Whose a test's samples are: one thread, on |cpu|.
auto context_on(u32 cpu) -> sampler::SampleContext {
    return sampler::SampleContext{.tid = 42, .pid = 0, .cpu = cpu, .timestamp = 1000};
}

auto append_to_string(void *context, char const *bytes, usize len) -> void {
    static_cast<std::string *>(context)->append(bytes, len);
}

class RecorderTest: public ::testing::Test {
  protected:
    void SetUp() override {
        for (usize cpu = 0; cpu < BUFFERS.size(); ++cpu) {
            BUFFERS[cpu].attach(SLOTS[cpu].data(), DEPTH);
        }
        ASSERT_TRUE(m_recorder.init({
            .buffer_lookup = &lookup,
            .buffer_lookup_context = nullptr,
            .num_cpus = static_cast<u32>(BUFFERS.size()),
        }));
    }

    /// Records a one-frame sample of |pc| on |cpu|.
    auto record_pc(u32 const cpu, u64 const pc) -> void {
        u64 const ips[] = {pc};
        m_recorder.record(context_on(cpu), ips, sampler::RecordFlags::None);
    }

    /// A recorder per test, as the kernel keeps one per session: nothing
    /// one test leaves behind can reach the next.
    sampler::Recorder m_recorder;
};

TEST_F(RecorderTest, RecordSampleGoesToTheContextsCpuBuffer) {
    record_pc(1, 0x1000);

    EXPECT_EQ(m_recorder.buffer_stats(0).total, 0u);
    EXPECT_EQ(m_recorder.buffer_stats(1).total, 1u);
    EXPECT_EQ(m_recorder.buffer_stats(1).retained, 1u);
}

TEST_F(RecorderTest, AFullBufferKeepsItsDepthAndCountsTheRest) {
    for (usize i = 0; i < DEPTH + 3; ++i) {
        record_pc(0, 0x1000 + i);
    }

    auto const stats = m_recorder.buffer_stats(0);
    EXPECT_EQ(stats.retained, DEPTH);
    EXPECT_EQ(stats.dropped, 3u);
    EXPECT_EQ(stats.total, DEPTH + 3);
}

TEST_F(RecorderTest, ABufferWithNoSlotsDropsEverything) {
    BUFFERS[1].attach(nullptr, 0);
    record_pc(1, 0x1000);

    auto const stats = m_recorder.buffer_stats(1);
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_EQ(stats.dropped, 1u);
}

TEST_F(RecorderTest, ResetEmptiesThePreviousRunsBuffers) {
    record_pc(0, 0x1000);
    m_recorder.note_unmatched(0);
    ASSERT_EQ(m_recorder.buffer_stats(0).total, 1u);

    m_recorder.reset();
    EXPECT_EQ(m_recorder.buffer_stats(0).total, 0u);
    EXPECT_EQ(m_recorder.buffer_stats(0).skipped, 0u);
}

TEST_F(RecorderTest, ExportDumpWritesTheDocumentedLineFraming) {
    record_pc(0, 1);

    std::string output;
    m_recorder.export_dump(sampler::LineWriter{&output, &append_to_string}, 0);

    auto const header_prefix = std::string(sampler::DUMP_HEADER_TAG) + " ";
    auto const record_prefix = std::string(sampler::DUMP_RECORD_TAG) + " ";

    ASSERT_GE(output.size(), header_prefix.size());
    EXPECT_EQ(output.compare(0, header_prefix.size(), header_prefix), 0);
    EXPECT_NE(output.find(record_prefix), std::string::npos);

    auto const header_line_end = output.find("\r\n");
    ASSERT_NE(header_line_end, std::string::npos);
    EXPECT_EQ(header_line_end - header_prefix.size(), sizeof(sampler::DumpHeader) * 2);
}

/// Collects copy_dump()'s pieces where their offsets say, and can be told
/// to refuse the piece at a given offset, as a user copy that faults would.
struct ByteSink final {
    std::string bytes;
    usize refuse_at = ~usize{0};
};

auto write_bytes(void *context, usize offset, void const *bytes, usize len) -> ours::Status {
    auto *sink = static_cast<ByteSink *>(context);
    if (offset == sink->refuse_at) {
        return ours::Status::InvArgs;
    }
    if (sink->bytes.size() < offset + len) {
        sink->bytes.resize(offset + len);
    }
    sink->bytes.replace(offset, len, static_cast<char const *>(bytes), len);
    return ours::Status::Ok;
}

TEST_F(RecorderTest, CopyDumpWritesTheHeaderThenEveryRecord) {
    record_pc(0, 0x10);
    record_pc(1, 0x20);
    record_pc(1, 0x30);

    auto const size = m_recorder.dump_size();
    EXPECT_EQ(size, sizeof(sampler::DumpHeader) + 3 * sizeof(sampler::SampleRecord));

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}, 0x1234), ours::Status::Ok);
    ASSERT_EQ(sink.bytes.size(), size);

    sampler::DumpHeader header = {};
    std::memcpy(&header, sink.bytes.data(), sizeof(header));
    EXPECT_EQ(header.magic, sampler::DUMP_MAGIC);
    EXPECT_EQ(header.num_records, 3u);
    EXPECT_EQ(header.num_cpus, 2u);
    EXPECT_EQ(header.load_base, 0x1234u);

    // CPU order: cpu0's one record, then cpu1's two, oldest first.
    u64 const pcs[] = {0x10, 0x20, 0x30};
    for (usize i = 0; i < 3; ++i) {
        sampler::SampleRecord record = {};
        std::memcpy(&record, sink.bytes.data() + sizeof(header) + i * sizeof(record),
                    sizeof(record));
        EXPECT_EQ(record.magic, sampler::SAMPLE_MAGIC);
        EXPECT_EQ(record.ips[0], pcs[i]);
    }
}

TEST_F(RecorderTest, CopyDumpStopsAtTheFirstRefusal) {
    record_pc(0, 0x10);
    record_pc(0, 0x20);

    ByteSink sink;
    sink.refuse_at = sizeof(sampler::DumpHeader);
    EXPECT_EQ(m_recorder.copy_dump({&sink, &write_bytes}, 0), ours::Status::InvArgs);
    EXPECT_EQ(sink.bytes.size(), sizeof(sampler::DumpHeader));
}






TEST_F(RecorderTest, RecordUserStackKeepsTheWalkedFrames) {
    u64 const ips[] = {0x10, 0x20, 0x30};
    m_recorder.record(context_on(1), ips, sampler::RecordFlags::FromUser);

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}, 0), ours::Status::Ok);
    sampler::SampleRecord record = {};
    std::memcpy(&record, sink.bytes.data() + sizeof(sampler::DumpHeader), sizeof(record));
    EXPECT_EQ(record.cpu, 1u);
    EXPECT_EQ(record.depth, 3u);
    EXPECT_EQ(record.flags, sampler::RecordFlags::FromUser);
    EXPECT_EQ(record.ips[2], 0x30u);
}





TEST_F(RecorderTest, RecordsSayTheirProcessAndTheHeaderCountsUnmatchedTicks) {
    auto context = context_on(0);
    context.pid = 0x2a;
    u64 const ips[] = {0x1000};
    m_recorder.record(context, ips, sampler::RecordFlags::None);
    m_recorder.note_unmatched(0);
    m_recorder.note_unmatched(1);

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}, 0), ours::Status::Ok);
    sampler::DumpHeader header = {};
    std::memcpy(&header, sink.bytes.data(), sizeof(header));
    EXPECT_EQ(header.version, 2u);
    EXPECT_EQ(header.num_records, 1u);
    EXPECT_EQ(header.num_unmatched, 2u);
    sampler::SampleRecord record = {};
    std::memcpy(&record, sink.bytes.data() + sizeof(sampler::DumpHeader), sizeof(record));
    EXPECT_EQ(record.pid, 0x2au);
}



} // namespace
