/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side coverage for prof::Sampler's own logic (state machine, buffer
// selection via the injected BufferLookup, dump encoding), backed by the
// fake prof::arch seam in fake_arch.cpp instead of a real kernel. Linking
// this executable at all is itself part of the point: it proves Sampler
// no longer needs ours::/gktl::/logz4::/CpuLocal to build and run.
#include <prof/format.hpp>
#include <prof/sampler.hpp>

#include <array>
#include <string>

#include "fake_arch.hpp"
#include <gtest/gtest.h>
#include <ours/status.hpp>

namespace {

std::array<prof::SampleBuffer, 2> g_buffers;

auto lookup(void *, u32 cpu) -> prof::SampleBuffer * {
    if (cpu >= g_buffers.size()) {
        return nullptr;
    }
    return &g_buffers[cpu];
}

class SamplerTest: public ::testing::Test {
  protected:
    void SetUp() override {
        prof_test::reset();
        for (auto &buffer: g_buffers) {
            buffer.reset();
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

TEST_F(SamplerTest, StartsAndStopsOnTheTimerSource) {
    EXPECT_FALSE(prof::Sampler::get().is_running());

    auto const status = prof::Sampler::get().start({.source = prof::Source::Timer, .period = 4});
    EXPECT_EQ(status, ours::Status::Ok);
    EXPECT_TRUE(prof::Sampler::get().is_running());
    EXPECT_EQ(prof::Sampler::get().active_source(), prof::Source::Timer);
    EXPECT_EQ(prof::Sampler::get().active_period(), 4u);

    prof::Sampler::get().stop();
    EXPECT_FALSE(prof::Sampler::get().is_running());
    EXPECT_EQ(prof_test::pmu_stop_calls(), 0); // The timer source never touches the PMU.
}

TEST_F(SamplerTest, PmuSourceFailsWithoutAnArchitecturalCounter) {
    prof_test::set_pmu_present(false);

    auto const status = prof::Sampler::get().start({.source = prof::Source::Pmu, .period = 1000});
    EXPECT_EQ(status, ours::Status::NotSupported);
    EXPECT_FALSE(prof::Sampler::get().is_running());
}

TEST_F(SamplerTest, PmuSourceProgramsThroughTheArchSeam) {
    prof_test::set_pmu_present(true);

    auto const status = prof::Sampler::get().start({.source = prof::Source::Pmu, .period = 1000});
    EXPECT_EQ(status, ours::Status::Ok);
    EXPECT_EQ(prof_test::pmu_start_calls(), 1);

    prof::Sampler::get().stop();
    EXPECT_EQ(prof_test::pmu_stop_calls(), 1);
}

TEST_F(SamplerTest, RecordSampleGoesToTheCurrentCpusBuffer) {
    prof_test::set_current_cpu(1);
    ASSERT_EQ(prof::Sampler::get().start({.source = prof::Source::Timer, .period = 1}),
              ours::Status::Ok);

    prof::Sampler::get().record_sample({
        .pc = 0x1000,
        .sp = 0,
        .bp = 0,
        .tid = 42,
        .from_uspace = true,
    });

    EXPECT_EQ(prof::Sampler::get().buffer_stats(0).total, 0u);
    EXPECT_EQ(prof::Sampler::get().buffer_stats(1).total, 1u);
    EXPECT_EQ(prof::Sampler::get().buffer_stats(1).retained, 1u);
}

TEST_F(SamplerTest, ExportDumpWritesTheDocumentedLineFraming) {
    prof_test::set_current_cpu(0);
    ASSERT_EQ(prof::Sampler::get().start({.source = prof::Source::Timer, .period = 1}),
              ours::Status::Ok);
    prof::Sampler::get().record_sample({.pc = 1, .sp = 0, .bp = 0, .tid = 0, .from_uspace = true});
    prof::Sampler::get().stop();

    prof::Sampler::get().export_dump();

    auto const &output = prof_test::debug_output();
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
