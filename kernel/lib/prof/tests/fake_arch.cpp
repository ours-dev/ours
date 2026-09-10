/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "fake_arch.hpp"

#include <prof/arch.hpp>

namespace {
bool g_pmu_present = false;
u32 g_current_cpu = 0;
u64 g_timestamp = 0;
std::optional<prof::SampleContext> g_next_context;
int g_pmu_start_calls = 0;
int g_pmu_stop_calls = 0;
std::string g_debug_output;
} // namespace

namespace prof_test {

auto reset() -> void {
    g_pmu_present = false;
    g_current_cpu = 0;
    g_timestamp = 0;
    g_next_context = std::nullopt;
    g_pmu_start_calls = 0;
    g_pmu_stop_calls = 0;
    g_debug_output.clear();
}

auto set_pmu_present(bool present) -> void {
    g_pmu_present = present;
}

auto set_current_cpu(u32 cpu) -> void {
    g_current_cpu = cpu;
}

auto set_timestamp(u64 ts) -> void {
    g_timestamp = ts;
}

auto set_next_sample_context(std::optional<prof::SampleContext> context) -> void {
    g_next_context = context;
}

auto pmu_start_calls() -> int {
    return g_pmu_start_calls;
}

auto pmu_stop_calls() -> int {
    return g_pmu_stop_calls;
}

auto debug_output() -> std::string const & {
    return g_debug_output;
}

} // namespace prof_test

auto prof::arch::pmu_present() -> bool {
    return g_pmu_present;
}

auto prof::arch::start_pmu_sampling(u64) -> ours::Status {
    ++g_pmu_start_calls;
    return ours::Status::Ok;
}

auto prof::arch::stop_pmu_sampling() -> void {
    ++g_pmu_stop_calls;
}

auto prof::arch::capture_sample_context() -> std::optional<prof::SampleContext> {
    return g_next_context;
}

auto prof::arch::current_cpu() -> u32 {
    return g_current_cpu;
}

auto prof::arch::timestamp() -> u64 {
    return g_timestamp;
}

auto prof::arch::debug_write(char const *bytes, usize len) -> void {
    g_debug_output.append(bytes, len);
}
