/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// profile: samples the kernel and itself while it runs a workload of system
// calls and of user-space arithmetic, and prints the samples for the host's
// prof-fold.
//
// It is the sampling profiler driven from user space, end to end: the
// SAMPLING resource it was started with gives it a session (sampler_create),
// which it starts, keeps busy, stops and reads (sampler_start, _stop,
// _read). The dump goes to the console as one `PROF1 <hex>` line for the
// header and one `SAMPB <base64>` line per record:
//
//   prof-fold --elf vmours --samples console.txt > folded.txt
//
// Exits 0 when the session recorded both kernel and user samples; 1 when it
// was started without the resource or a call failed; 2 when a target
// recorded nothing.
#include <ours/process_args.hpp>
#include <ours/sampler.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/vdso.hpp>

#include <string_view>

#include <cstring>

#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>

namespace {
/// Records each CPU keeps. At the kernel's 100 Hz tick, more than half a
/// second of samples.
constexpr u64 RECORDS_PER_CPU = 64;

/// The most CPUs a session can have a buffer for: the kernel's MP_MAX_CPUS.
constexpr usize MAX_CPUS = 16;

/// Where the dump is read to. Static rather than on the stack: at 16 CPUs it
/// is about 150 KiB.
u8 g_dump[sizeof(ours::SamplerDumpHeader) +
          MAX_CPUS * RECORDS_PER_CPU * sizeof(ours::SamplerRecord)];

auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({"profile: ", what, " = ", ours::to_string(status)});
}

/// The kernel work to sample: system calls, so that the time it takes is
/// the kernel's, which the kernel target records.
auto kernel_work() -> void {
    for (auto i = 0; i < 10'000; ++i) {
        auto vmo = ours::Uandle::Invalid;
        if (ours::vmo_create(4096, ours::VmoOptions::None, &vmo) == ours::Status::Ok) {
            auto _ = ours::handle_close(vmo);
        }
    }
}

/// The user work to sample: arithmetic that never enters the kernel, two
/// calls deep, so that a user record has frames to walk. The volatile
/// store is what keeps the compiler from folding the loop away.
[[gnu::noinline]]
auto spin_inner(u64 const rounds) -> u64 {
    static u64 volatile sink = 0;
    for (u64 i = 0; i < rounds; ++i) {
        sink = sink + i;
    }
    return sink;
}

[[gnu::noinline]]
auto user_work() -> u64 {
    u64 total = 0;
    for (auto i = 0; i < 20; ++i) {
        total += spin_inner(1'000'000);
    }
    return total;
}

/// Writes `PROF1 <hex of |bytes|>`: the header is 32 bytes, so its line is
/// 72, well inside one debug_write.
auto print_header(ours::SamplerDumpHeader const &header) -> void {
    static constexpr char DIGITS[] = "0123456789abcdef";
    char line[6 + 2 * sizeof(header) + 1];
    std::memcpy(line, "PROF1 ", 6);
    u8 bytes[sizeof(header)];
    std::memcpy(bytes, &header, sizeof(header));
    for (usize i = 0; i < sizeof(bytes); ++i) {
        line[6 + 2 * i] = DIGITS[bytes[i] >> 4];
        line[6 + 2 * i + 1] = DIGITS[bytes[i] & 0xf];
    }
    line[sizeof(line) - 1] = '\n';
    auto _ = ours::debug_write(line, sizeof(line));
}

/// Writes `SAMPB <base64 of |record|>`.
///
/// Base64, not the hex the header uses, because of the size: a record is 152
/// bytes, 304 hex digits, and one debug_write takes at most MAX_DEBUG_WRITE
/// (256) bytes. Splitting a line over two writes would let another CPU's
/// console output land in the middle of it. In base64 the record is 204
/// characters and the line 211, one write.
auto print_record(ours::SamplerRecord const &record) -> void {
    static constexpr char ALPHABET[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr usize ENCODED = (sizeof(record) + 2) / 3 * 4;
    static_assert(6 + ENCODED + 1 <= ours::MAX_DEBUG_WRITE);

    u8 bytes[sizeof(record)];
    std::memcpy(bytes, &record, sizeof(record));
    char line[6 + ENCODED + 1];
    std::memcpy(line, "SAMPB ", 6);
    auto out = usize{6};
    for (usize i = 0; i < sizeof(bytes); i += 3) {
        auto const remaining = sizeof(bytes) - i;
        u32 group = u32{bytes[i]} << 16;
        if (remaining > 1) {
            group |= u32{bytes[i + 1]} << 8;
        }
        if (remaining > 2) {
            group |= u32{bytes[i + 2]};
        }
        line[out++] = ALPHABET[(group >> 18) & 0x3f];
        line[out++] = ALPHABET[(group >> 12) & 0x3f];
        // The last group pads with '=' for the bytes it does not have.
        line[out++] = '=';
        line[out++] = '=';
        if (remaining > 1) {
            line[out - 2] = ALPHABET[(group >> 6) & 0x3f];
        }
        if (remaining > 2) {
            line[out - 1] = ALPHABET[group & 0x3f];
        }
    }
    line[out++] = '\n';
    auto _ = ours::debug_write(line, out);
}
} // namespace

auto main(int, char **) -> int {
    using ours::Status;

    auto const resource = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                   u16(ours::RSRC_SYSTEM_SAMPLING_BASE));
    if (resource == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("profile: started without the SAMPLING resource");
        return 1;
    }

    auto const config = ours::SamplerConfig{
        .period = 1'000'000,
        .buffer_size = RECORDS_PER_CPU * sizeof(ours::SamplerRecord),
        .targets = ours::SamplerTargets::Kernel | ours::SamplerTargets::User,
        .reserved = 0,
    };
    auto sampler = ours::Uandle::Invalid;
    if (auto const status = ours::sampler_create(resource, 0, &config, &sampler);
        status != Status::Ok) {
        report("sampler_create", status);
        return 1;
    }
    if (auto const status = ours::sampler_start(sampler); status != Status::Ok) {
        report("sampler_start", status);
        return 1;
    }
    kernel_work();
    auto _ = user_work();
    if (auto const status = ours::sampler_stop(sampler); status != Status::Ok) {
        report("sampler_stop", status);
        return 1;
    }

    usize actual = 0;
    if (auto const status = ours::sampler_read(sampler, g_dump, sizeof(g_dump), &actual);
        status != Status::Ok) {
        report("sampler_read", status);
        return 1;
    }
    // The session's work is done; closing its handle ends it, so another
    // program may create one.
    auto _ = ours::handle_close(sampler);

    ours::SamplerDumpHeader header{};
    std::memcpy(&header, g_dump, sizeof(header));
    print_header(header);
    u32 user = 0;
    for (u32 i = 0; i < header.num_records; ++i) {
        ours::SamplerRecord record{};
        std::memcpy(&record, g_dump + header.header_size + usize{i} * header.record_size,
                    sizeof(record));
        if (!!(record.flags & ours::SamplerRecordFlags::FromUser)) {
            ++user;
        }
        print_record(record);
    }

    char kernel_count[10];
    char user_count[10];
    char dropped[10];
    auto _ =
        ort::debug_line({"profile: ", ort::decimal(header.num_records - user, kernel_count),
                         " kernel samples, ", ort::decimal(user, user_count), " user samples, ",
                         ort::decimal(header.num_dropped, dropped), " dropped"});
    if (user == 0 || user == header.num_records) {
        return 2;
    }
    return 0;
}
