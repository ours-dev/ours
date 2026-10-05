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
#include <string>

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
        prof::Sampler::get().init({
            .buffer_lookup = &lookup,
            .buffer_lookup_context = nullptr,
            .num_cpus = static_cast<u32>(g_buffers.size()),
        });
    }

    void TearDown() override {
        prof::Sampler::get().stop();
    }
};

TEST_F(SamplerTest, StartsAndStops) {
    EXPECT_FALSE(prof::Sampler::get().is_running());

    EXPECT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    EXPECT_TRUE(prof::Sampler::get().is_running());

    prof::Sampler::get().stop();
    EXPECT_FALSE(prof::Sampler::get().is_running());
}

TEST_F(SamplerTest, StartingTwiceIsRefused) {
    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    EXPECT_EQ(prof::Sampler::get().start(), ours::Status::BadState);
    EXPECT_TRUE(prof::Sampler::get().is_running());
}

TEST_F(SamplerTest, RecordSampleGoesToTheContextsCpuBuffer) {
    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);

    prof::Sampler::get().record_sample(user_sample(1, 0x1000));

    EXPECT_EQ(prof::Sampler::get().buffer_stats(0).total, 0u);
    EXPECT_EQ(prof::Sampler::get().buffer_stats(1).total, 1u);
    EXPECT_EQ(prof::Sampler::get().buffer_stats(1).retained, 1u);
}

TEST_F(SamplerTest, AFullBufferKeepsItsDepthAndCountsTheRest) {
    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    for (usize i = 0; i < DEPTH + 3; ++i) {
        prof::Sampler::get().record_sample(user_sample(0, 0x1000 + i));
    }

    auto const stats = prof::Sampler::get().buffer_stats(0);
    EXPECT_EQ(stats.retained, DEPTH);
    EXPECT_EQ(stats.dropped, 3u);
    EXPECT_EQ(stats.total, DEPTH + 3);
}

TEST_F(SamplerTest, ABufferWithNoSlotsDropsEverything) {
    g_buffers[1].attach(nullptr, 0);
    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    prof::Sampler::get().record_sample(user_sample(1, 0x1000));

    auto const stats = prof::Sampler::get().buffer_stats(1);
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_EQ(stats.dropped, 1u);
}

TEST_F(SamplerTest, SamplesAreRefusedWhileStopped) {
    prof::Sampler::get().record_sample(user_sample(0, 0x1000));
    EXPECT_EQ(prof::Sampler::get().buffer_stats(0).total, 0u);
}

TEST_F(SamplerTest, StartEmptiesThePreviousSessionsBuffers) {
    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    prof::Sampler::get().record_sample(user_sample(0, 0x1000));
    prof::Sampler::get().stop();
    ASSERT_EQ(prof::Sampler::get().buffer_stats(0).total, 1u);

    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    EXPECT_EQ(prof::Sampler::get().buffer_stats(0).total, 0u);
}

TEST_F(SamplerTest, ExportDumpWritesTheDocumentedLineFraming) {
    ASSERT_EQ(prof::Sampler::get().start(), ours::Status::Ok);
    prof::Sampler::get().record_sample(user_sample(0, 1));
    prof::Sampler::get().stop();

    std::string output;
    prof::Sampler::get().export_dump(prof::LineWriter{&output, &append_to_string});

    auto const header_prefix = std::string(prof::DUMP_HEADER_TAG) + " ";
    auto const record_prefix = std::string(prof::DUMP_RECORD_TAG) + " ";

    ASSERT_GE(output.size(), header_prefix.size());
    EXPECT_EQ(output.compare(0, header_prefix.size(), header_prefix), 0);
    EXPECT_NE(output.find(record_prefix), std::string::npos);

    auto const header_line_end = output.find("\r\n");
    ASSERT_NE(header_line_end, std::string::npos);
    EXPECT_EQ(header_line_end - header_prefix.size(), sizeof(prof::DumpHeader) * 2);
}

} // namespace
