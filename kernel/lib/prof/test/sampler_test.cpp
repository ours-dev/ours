/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side coverage for prof::Sampler's own logic (state machine, buffer
// selection, dump encoding) with no kernel underneath it. Every fact a sample
// needs arrives in its SampleContext and every dump line leaves through a
// LineWriter, so there is nothing to fake: linking this executable at all
// proves Sampler needs nothing from ours::/gktl::/logz4::/CpuLocal or from an
// architecture.
#include <prof/format.hpp>
#include <prof/sampler.hpp>

#include <array>
#include <map>
#include <string>

#include <cstring>

#include <gtest/gtest.h>
#include <ours/status.hpp>

namespace {

/// Two CPUs' worth of buffers, four records deep: deep enough for every test
/// but the one that fills a buffer on purpose.
constexpr usize DEPTH = 4;
std::array<std::array<prof::SampleRecord, DEPTH>, 2> g_slots;
std::array<prof::SampleBuffer, 2> g_buffers;

auto lookup(void *, u32 cpu) -> prof::SampleBuffer * {
    if (cpu >= g_buffers.size()) {
        return nullptr;
    }
    return &g_buffers[cpu];
}

/// A user-space sample: it is recorded at depth 1 without unwinding, so the
/// test needs no unwind tables.
auto user_sample(u32 cpu, u64 pc) -> prof::SampleContext {
    return prof::SampleContext{
        .pc = pc,
        .sp = 0,
        .bp = 0,
        .tid = 42,
        .cpu = cpu,
        .timestamp = 1000,
        .from_uspace = true,
    };
}

auto append_to_string(void *context, char const *bytes, usize len) -> void {
    static_cast<std::string *>(context)->append(bytes, len);
}

class SamplerTest: public ::testing::Test {
  protected:
    void SetUp() override {
        for (usize cpu = 0; cpu < g_buffers.size(); ++cpu) {
            g_buffers[cpu].attach(g_slots[cpu].data(), DEPTH);
        }
        m_sampler.init({
            .buffer_lookup = &lookup,
            .buffer_lookup_context = nullptr,
            .num_cpus = static_cast<u32>(g_buffers.size()),
        });
    }

    void TearDown() override {
        m_sampler.stop();
    }

    /// A sampler per test, as the kernel keeps one per session: nothing
    /// one test leaves behind can reach the next.
    prof::Sampler m_sampler;
};

TEST_F(SamplerTest, StartsAndStops) {
    EXPECT_FALSE(m_sampler.is_running());

    EXPECT_EQ(m_sampler.start(), ours::Status::Ok);
    EXPECT_TRUE(m_sampler.is_running());

    m_sampler.stop();
    EXPECT_FALSE(m_sampler.is_running());
}

TEST_F(SamplerTest, StartingTwiceIsRefused) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    EXPECT_EQ(m_sampler.start(), ours::Status::BadState);
    EXPECT_TRUE(m_sampler.is_running());
}

TEST_F(SamplerTest, RecordSampleGoesToTheContextsCpuBuffer) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);

    m_sampler.record_sample(user_sample(1, 0x1000));

    EXPECT_EQ(m_sampler.buffer_stats(0).total, 0u);
    EXPECT_EQ(m_sampler.buffer_stats(1).total, 1u);
    EXPECT_EQ(m_sampler.buffer_stats(1).retained, 1u);
}

TEST_F(SamplerTest, AFullBufferKeepsItsDepthAndCountsTheRest) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    for (usize i = 0; i < DEPTH + 3; ++i) {
        m_sampler.record_sample(user_sample(0, 0x1000 + i));
    }

    auto const stats = m_sampler.buffer_stats(0);
    EXPECT_EQ(stats.retained, DEPTH);
    EXPECT_EQ(stats.dropped, 3u);
    EXPECT_EQ(stats.total, DEPTH + 3);
}

TEST_F(SamplerTest, ABufferWithNoSlotsDropsEverything) {
    g_buffers[1].attach(nullptr, 0);
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    m_sampler.record_sample(user_sample(1, 0x1000));

    auto const stats = m_sampler.buffer_stats(1);
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_EQ(stats.dropped, 1u);
}

TEST_F(SamplerTest, SamplesAreRefusedWhileStopped) {
    m_sampler.record_sample(user_sample(0, 0x1000));
    EXPECT_EQ(m_sampler.buffer_stats(0).total, 0u);
}

TEST_F(SamplerTest, StartEmptiesThePreviousSessionsBuffers) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    m_sampler.record_sample(user_sample(0, 0x1000));
    m_sampler.stop();
    ASSERT_EQ(m_sampler.buffer_stats(0).total, 1u);

    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    EXPECT_EQ(m_sampler.buffer_stats(0).total, 0u);
}

TEST_F(SamplerTest, ExportDumpWritesTheDocumentedLineFraming) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    m_sampler.record_sample(user_sample(0, 1));
    m_sampler.stop();

    std::string output;
    m_sampler.export_dump(prof::LineWriter{&output, &append_to_string});

    auto const header_prefix = std::string(prof::DUMP_HEADER_TAG) + " ";
    auto const record_prefix = std::string(prof::DUMP_RECORD_TAG) + " ";

    ASSERT_GE(output.size(), header_prefix.size());
    EXPECT_EQ(output.compare(0, header_prefix.size(), header_prefix), 0);
    EXPECT_NE(output.find(record_prefix), std::string::npos);

    auto const header_line_end = output.find("\r\n");
    ASSERT_NE(header_line_end, std::string::npos);
    EXPECT_EQ(header_line_end - header_prefix.size(), sizeof(prof::DumpHeader) * 2);
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

TEST_F(SamplerTest, CopyDumpWritesTheHeaderThenEveryRecord) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    m_sampler.record_sample(user_sample(0, 0x10));
    m_sampler.record_sample(user_sample(1, 0x20));
    m_sampler.record_sample(user_sample(1, 0x30));
    m_sampler.stop();

    auto const size = m_sampler.dump_size();
    EXPECT_EQ(size, sizeof(prof::DumpHeader) + 3 * sizeof(prof::SampleRecord));

    ByteSink sink;
    ASSERT_EQ(m_sampler.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    ASSERT_EQ(sink.bytes.size(), size);

    prof::DumpHeader header = {};
    std::memcpy(&header, sink.bytes.data(), sizeof(header));
    EXPECT_EQ(header.magic, prof::DUMP_MAGIC);
    EXPECT_EQ(header.num_records, 3u);
    EXPECT_EQ(header.num_cpus, 2u);

    // CPU order: cpu0's one record, then cpu1's two, oldest first.
    u64 const pcs[] = {0x10, 0x20, 0x30};
    for (usize i = 0; i < 3; ++i) {
        prof::SampleRecord record = {};
        std::memcpy(&record, sink.bytes.data() + sizeof(header) + i * sizeof(record),
                    sizeof(record));
        EXPECT_EQ(record.magic, prof::SAMPLE_MAGIC);
        EXPECT_EQ(record.ips[0], pcs[i]);
    }
}

TEST_F(SamplerTest, CopyDumpStopsAtTheFirstRefusal) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    m_sampler.record_sample(user_sample(0, 0x10));
    m_sampler.record_sample(user_sample(0, 0x20));
    m_sampler.stop();

    ByteSink sink;
    sink.refuse_at = sizeof(prof::DumpHeader);
    EXPECT_EQ(m_sampler.copy_dump({&sink, &write_bytes}), ours::Status::InvArgs);
    EXPECT_EQ(sink.bytes.size(), sizeof(prof::DumpHeader));
}

/// A stack in a std::map, as the frame-pointer walk's reader sees it:
/// addresses not in the map are not there.
struct FakeStack final {
    std::map<u64, u64> words;
};

auto read_fake_stack(void *context, u64 address, u64 *out) -> bool {
    auto const &stack = static_cast<FakeStack const *>(context)->words;
    auto const it = stack.find(address);
    if (it == stack.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

TEST(FrameWalk, FollowsTheChainOutwards) {
    // Three frames: 0x1000 -> 0x1020 -> 0x1040, the last one's caller is 0.
    FakeStack stack;
    stack.words = {
        {0x1000, 0x1020}, {0x1008, 0xa1}, // frame 1: next fp, return address
        {0x1020, 0x1040}, {0x1028, 0xa2}, // frame 2
        {0x1040, 0x0000}, {0x1048, 0x0},  // the outermost: no return address
    };
    u64 ips[8] = {};
    auto const depth =
        prof::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips));
    ASSERT_EQ(depth, 3u);
    EXPECT_EQ(ips[0], 0xa0u);
    EXPECT_EQ(ips[1], 0xa1u);
    EXPECT_EQ(ips[2], 0xa2u);
}

TEST(FrameWalk, StopsAtMemoryThatIsNotThere) {
    FakeStack stack;
    stack.words = {{0x1000, 0x2000}, {0x1008, 0xa1}};
    u64 ips[8] = {};
    EXPECT_EQ(
        prof::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        2u);
}

TEST(FrameWalk, StopsAtAChainThatDoesNotMoveUp) {
    // A frame that points at itself would loop for ever.
    FakeStack stack;
    stack.words = {{0x1000, 0x1000}, {0x1008, 0xa1}};
    u64 ips[8] = {};
    EXPECT_EQ(
        prof::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        2u);
}

TEST(FrameWalk, RefusesAMisalignedOrNullFramePointer) {
    FakeStack stack;
    u64 ips[8] = {};
    EXPECT_EQ(prof::walk_frame_pointers(0xa0, 0x1003, {&stack, &read_fake_stack}, ips, 8), 1u);
    EXPECT_EQ(prof::walk_frame_pointers(0xa0, 0, {&stack, &read_fake_stack}, ips, 8), 1u);
}

TEST(FrameWalk, KeepsAtMostTheRoomGiven) {
    FakeStack stack;
    for (u64 frame = 0; frame < 8; ++frame) {
        stack.words[0x1000 + frame * 0x20] = 0x1000 + (frame + 1) * 0x20;
        stack.words[0x1008 + frame * 0x20] = 0xb0 + frame;
    }
    u64 ips[3] = {};
    EXPECT_EQ(
        prof::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        3u);
}

TEST_F(SamplerTest, RecordUserStackKeepsTheWalkedFrames) {
    ASSERT_EQ(m_sampler.start(), ours::Status::Ok);
    u64 const ips[] = {0x10, 0x20, 0x30};
    m_sampler.record_user_stack(user_sample(1, 0), ips, std::size(ips));
    m_sampler.stop();

    ByteSink sink;
    ASSERT_EQ(m_sampler.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    prof::SampleRecord record = {};
    std::memcpy(&record, sink.bytes.data() + sizeof(prof::DumpHeader), sizeof(record));
    EXPECT_EQ(record.cpu, 1u);
    EXPECT_EQ(record.depth, 3u);
    EXPECT_EQ(record.flags, prof::RecordFlags::FromUser);
    EXPECT_EQ(record.ips[2], 0x30u);
}

TEST_F(SamplerTest, RecordUserStackIsRefusedWhileStopped) {
    u64 const ips[] = {0x10};
    m_sampler.record_user_stack(user_sample(0, 0), ips, std::size(ips));
    EXPECT_EQ(m_sampler.buffer_stats(0).total, 0u);
}

} // namespace
