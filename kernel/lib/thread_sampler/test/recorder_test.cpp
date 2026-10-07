/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side coverage for sampler::Recorder's own logic (state machine, buffer
// selection, dump encoding) with no kernel underneath it. Every fact a sample
// needs arrives in its SampleContext and every dump line leaves through a
// LineWriter, so there is nothing to fake: linking this executable at all
// proves Recorder needs nothing from ours::/gktl::/logz4::/CpuLocal or from an
// architecture.
#include <thread_sampler/format.hpp>
#include <thread_sampler/recorder.hpp>

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
std::array<std::array<sampler::SampleRecord, DEPTH>, 2> SLOTS;
std::array<sampler::SampleBuffer, 2> BUFFERS;

auto lookup(void *, u32 cpu) -> sampler::SampleBuffer * {
    if (cpu >= BUFFERS.size()) {
        return nullptr;
    }
    return &BUFFERS[cpu];
}

/// A user-space sample: it is recorded at depth 1 without unwinding, so the
/// test needs no unwind tables.
auto user_sample(u32 cpu, u64 pc) -> sampler::SampleContext {
    return sampler::SampleContext{
        .pc = pc,
        .sp = 0,
        .bp = 0,
        .tid = 42,
        .pid = 0,
        .cpu = cpu,
        .timestamp = 1000,
        .stack_base = 0,
        .stack_top = 0,
        .from_uspace = true,
    };
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
        m_recorder.init({
            .buffer_lookup = &lookup,
            .buffer_lookup_context = nullptr,
            .num_cpus = static_cast<u32>(BUFFERS.size()),
        });
    }

    /// A recorder per test, as the kernel keeps one per session: nothing
    /// one test leaves behind can reach the next.
    sampler::Recorder m_recorder;
};

TEST_F(RecorderTest, RecordSampleGoesToTheContextsCpuBuffer) {
    m_recorder.record_sample(user_sample(1, 0x1000));

    EXPECT_EQ(m_recorder.buffer_stats(0).total, 0u);
    EXPECT_EQ(m_recorder.buffer_stats(1).total, 1u);
    EXPECT_EQ(m_recorder.buffer_stats(1).retained, 1u);
}

TEST_F(RecorderTest, AFullBufferKeepsItsDepthAndCountsTheRest) {
    for (usize i = 0; i < DEPTH + 3; ++i) {
        m_recorder.record_sample(user_sample(0, 0x1000 + i));
    }

    auto const stats = m_recorder.buffer_stats(0);
    EXPECT_EQ(stats.retained, DEPTH);
    EXPECT_EQ(stats.dropped, 3u);
    EXPECT_EQ(stats.total, DEPTH + 3);
}

TEST_F(RecorderTest, ABufferWithNoSlotsDropsEverything) {
    BUFFERS[1].attach(nullptr, 0);
    m_recorder.record_sample(user_sample(1, 0x1000));

    auto const stats = m_recorder.buffer_stats(1);
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_EQ(stats.dropped, 1u);
}

TEST_F(RecorderTest, ResetEmptiesThePreviousRunsBuffers) {
    m_recorder.record_sample(user_sample(0, 0x1000));
    m_recorder.note_unmatched(0);
    ASSERT_EQ(m_recorder.buffer_stats(0).total, 1u);

    m_recorder.reset();
    EXPECT_EQ(m_recorder.buffer_stats(0).total, 0u);
    EXPECT_EQ(m_recorder.buffer_stats(0).skipped, 0u);
}

TEST_F(RecorderTest, ExportDumpWritesTheDocumentedLineFraming) {
    m_recorder.record_sample(user_sample(0, 1));

    std::string output;
    m_recorder.export_dump(prof::LineWriter{&output, &append_to_string});

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
    m_recorder.record_sample(user_sample(0, 0x10));
    m_recorder.record_sample(user_sample(1, 0x20));
    m_recorder.record_sample(user_sample(1, 0x30));

    auto const size = m_recorder.dump_size();
    EXPECT_EQ(size, sizeof(sampler::DumpHeader) + 3 * sizeof(sampler::SampleRecord));

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    ASSERT_EQ(sink.bytes.size(), size);

    sampler::DumpHeader header = {};
    std::memcpy(&header, sink.bytes.data(), sizeof(header));
    EXPECT_EQ(header.magic, sampler::DUMP_MAGIC);
    EXPECT_EQ(header.num_records, 3u);
    EXPECT_EQ(header.num_cpus, 2u);

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
    m_recorder.record_sample(user_sample(0, 0x10));
    m_recorder.record_sample(user_sample(0, 0x20));

    ByteSink sink;
    sink.refuse_at = sizeof(sampler::DumpHeader);
    EXPECT_EQ(m_recorder.copy_dump({&sink, &write_bytes}), ours::Status::InvArgs);
    EXPECT_EQ(sink.bytes.size(), sizeof(sampler::DumpHeader));
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
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips));
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
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        2u);
}

TEST(FrameWalk, StopsAtAChainThatDoesNotMoveUp) {
    // A frame that points at itself would loop for ever.
    FakeStack stack;
    stack.words = {{0x1000, 0x1000}, {0x1008, 0xa1}};
    u64 ips[8] = {};
    EXPECT_EQ(
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        2u);
}

TEST(FrameWalk, RefusesAMisalignedOrNullFramePointer) {
    FakeStack stack;
    u64 ips[8] = {};
    EXPECT_EQ(sampler::walk_frame_pointers(0xa0, 0x1003, {&stack, &read_fake_stack}, ips, 8), 1u);
    EXPECT_EQ(sampler::walk_frame_pointers(0xa0, 0, {&stack, &read_fake_stack}, ips, 8), 1u);
}

TEST(FrameWalk, KeepsAtMostTheRoomGiven) {
    FakeStack stack;
    for (u64 frame = 0; frame < 8; ++frame) {
        stack.words[0x1000 + frame * 0x20] = 0x1000 + (frame + 1) * 0x20;
        stack.words[0x1008 + frame * 0x20] = 0xb0 + frame;
    }
    u64 ips[3] = {};
    EXPECT_EQ(
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        3u);
}

TEST_F(RecorderTest, RecordUserStackKeepsTheWalkedFrames) {
    u64 const ips[] = {0x10, 0x20, 0x30};
    m_recorder.record_user_stack(user_sample(1, 0), ips, std::size(ips));

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    sampler::SampleRecord record = {};
    std::memcpy(&record, sink.bytes.data() + sizeof(sampler::DumpHeader), sizeof(record));
    EXPECT_EQ(record.cpu, 1u);
    EXPECT_EQ(record.depth, 3u);
    EXPECT_EQ(record.flags, sampler::RecordFlags::FromUser);
    EXPECT_EQ(record.ips[2], 0x30u);
}

TEST(StackWindow, IsEmptyUnlessTheStackPointerIsOnTheStack) {
    constexpr u64 BASE = 0x7000'0000;
    constexpr u64 TOP = BASE + 0x4000;
    EXPECT_FALSE(sampler::StackWindow::on_stack(TOP - 0x100, BASE, TOP).empty());
    EXPECT_TRUE(sampler::StackWindow::on_stack(BASE - 8, BASE, TOP).empty());
    EXPECT_TRUE(sampler::StackWindow::on_stack(TOP, BASE, TOP).empty()); // top is exclusive
    EXPECT_TRUE(sampler::StackWindow::on_stack(0x1000, 0, 0).empty());   // bounds unknown
    EXPECT_TRUE(sampler::StackWindow::on_stack(TOP - 8, TOP, BASE).empty());
}

TEST(StackWindow, ContainsWordsFromTheStackPointerToTheTop) {
    constexpr u64 BASE = 0x7000'0000;
    constexpr u64 TOP = BASE + 0x4000;
    constexpr u64 SP = TOP - 0x100;
    auto const window = sampler::StackWindow::on_stack(SP, BASE, TOP);

    EXPECT_TRUE(window.contains(SP));
    EXPECT_TRUE(window.contains(TOP - 8)); // the last whole word
    EXPECT_FALSE(window.contains(TOP));    // past the top
    EXPECT_FALSE(window.contains(SP - 8)); // below the stack pointer: not this frame's caller
    EXPECT_FALSE(window.contains(SP + 4)); // misaligned
    EXPECT_FALSE(window.contains(BASE));   // on the stack, but below sp
}

TEST(StackWindow, RefusesAWordThatWouldWrapPastTheTopOfTheAddressSpace) {
    constexpr u64 TOP = 0xFFFF'FFFF'FFFF'F000;
    constexpr u64 BASE = TOP - 0x4000;
    auto const window = sampler::StackWindow::on_stack(TOP - 0x100, BASE, TOP);
    // address + 8 wraps to 0 here; a check written that way would pass it.
    EXPECT_FALSE(window.contains(0xFFFF'FFFF'FFFF'FFF8));
    EXPECT_TRUE(window.contains(TOP - 8));
}

TEST(StackWindow, AWindowNarrowerThanAWordContainsNothing) {
    constexpr u64 BASE = 0x7000'0000;
    constexpr u64 TOP = BASE + 0x4000;
    auto const window = sampler::StackWindow::on_stack(TOP - 4, BASE, TOP);
    EXPECT_FALSE(window.empty());
    EXPECT_FALSE(window.contains(TOP - 4));
    EXPECT_FALSE(window.contains(TOP - 8));
}

TEST_F(RecorderTest, RecordsSayTheirProcessAndTheHeaderCountsUnmatchedTicks) {
    auto context = user_sample(0, 0x1000);
    context.pid = 0x2a;
    m_recorder.record_sample(context);
    m_recorder.note_unmatched(0);
    m_recorder.note_unmatched(1);

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    sampler::DumpHeader header = {};
    std::memcpy(&header, sink.bytes.data(), sizeof(header));
    EXPECT_EQ(header.version, 2u);
    EXPECT_EQ(header.num_records, 1u);
    EXPECT_EQ(header.num_unmatched, 2u);
    sampler::SampleRecord record = {};
    std::memcpy(&record, sink.bytes.data() + sizeof(sampler::DumpHeader), sizeof(record));
    EXPECT_EQ(record.pid, 0x2au);
}

TEST_F(RecorderTest, AFrameWithNoUnwindInfoEndsTheStackOnlyAtARoot) {
    // No unwind tables at all, so every kernel step finds no FDE: whether a
    // record is whole depends only on where that frame is.
    constexpr u64 TOP = 0x8000'0000;
    constexpr u64 BASE = TOP - 0x4000;
    constexpr u64 ROOT = 0x5000;
    constexpr u64 ROOT_DEPTH = 176;
    sampler::StackRoot const roots[] = {{ROOT, ROOT_DEPTH}};
    m_recorder.init({
        .buffer_lookup = &lookup,
        .buffer_lookup_context = nullptr,
        .num_cpus = static_cast<u32>(BUFFERS.size()),
        .stack_roots = roots,
        .stack_root_count = 1,
    });
    auto const kernel_at = [](u64 const pc, u64 const sp) {
        auto context = user_sample(0, pc);
        context.from_uspace = false;
        context.sp = sp;
        context.stack_base = BASE;
        context.stack_top = TOP;
        return context;
    };
    m_recorder.record_sample(kernel_at(ROOT, TOP - ROOT_DEPTH));      // the root itself
    m_recorder.record_sample(kernel_at(0x6000, TOP - ROOT_DEPTH));    // other code, same depth
    m_recorder.record_sample(kernel_at(ROOT, TOP - ROOT_DEPTH - 64)); // the stub, nested deeper

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    sampler::RecordFlags const want[] = {sampler::RecordFlags::None,
                                         sampler::RecordFlags::Truncated,
                                         sampler::RecordFlags::Truncated};
    for (usize i = 0; i < 3; ++i) {
        sampler::SampleRecord record = {};
        std::memcpy(&record, sink.bytes.data() + sizeof(sampler::DumpHeader) + i * sizeof(record),
                    sizeof(record));
        EXPECT_EQ(record.depth, 1u) << i;
        EXPECT_EQ(record.flags, want[i]) << i;
    }
}

TEST_F(RecorderTest, AKernelSampleOffItsStackIsKeptAsAPrefix) {
    auto context = user_sample(0, 0x10);
    context.from_uspace = false; // a kernel pc, with no stack bounds given
    m_recorder.record_sample(context);

    ByteSink sink;
    ASSERT_EQ(m_recorder.copy_dump({&sink, &write_bytes}), ours::Status::Ok);
    sampler::SampleRecord record = {};
    std::memcpy(&record, sink.bytes.data() + sizeof(sampler::DumpHeader), sizeof(record));
    EXPECT_EQ(record.depth, 1u);
    EXPECT_EQ(record.flags, sampler::RecordFlags::Truncated);
}

} // namespace
