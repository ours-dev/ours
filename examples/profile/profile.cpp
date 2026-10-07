/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// profile: samples the kernel and itself while it runs a workload of system
// calls and of user-space arithmetic, and prints the samples for the host's
// prof-fold.
//
// It is the sampling profiler driven from user space, end to end: the
// Sampling resource, which it reaches at /svc/ours.resource.Sampling in the
// namespace it was started with, gives it a session (sampler_create),
// which it starts, keeps busy, stops and reads (sampler_start, _stop,
// _read). The dump goes to the console as one `PROF1 <hex>` line for the
// header and one `SAMPB <base64>` line per record:
//
//   prof-fold --elf vmours --samples console.txt > folded.txt
//
// Exits 0 when the session recorded both kernel and user samples; 1 when the
// resource could not be had or a call failed; 2 when a target recorded
// nothing.
#include <ours/sampler.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <string_view>

#include <cstring>

#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>

namespace {
/// Records each CPU keeps. At the kernel's 100 Hz tick, more than half a
/// second of samples.
constexpr u64 RECORDS_PER_CPU = 64;

/// The most CPUs a session can have a buffer for: the kernel's MP_MAX_CPUS.
constexpr usize MAX_CPUS = 16;

/// Where the dump is read to. Static rather than on the stack: at 16 CPUs it
/// is about 150 KiB.
u8 DUMP[sizeof(ours::SamplerDumpHeader) + MAX_CPUS * RECORDS_PER_CPU * sizeof(ours::SamplerRecord)];

auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({"profile: ", what, " = ", ours::to_string(status)});
}

/// How many times the session runs one round of kernel work and then one of
/// user work.
///
/// Alternating many short rounds, rather than all the system calls and then
/// all the arithmetic, keeps both kinds of sample in the buffers when they
/// fill before the work is done. That happens: the dbg image has run this
/// workload on one CPU fifty to a hundred times slower than usual, and a run
/// that made its 10,000 system calls first then filled that CPU's
/// RECORDS_PER_CPU with kernel samples and dropped every user one but one.
/// A round is short enough that the first one reaches its user work within
/// a CPU's records even at that speed. The total work is what one long run
/// of each did.
constexpr auto ROUNDS = 40;

/// One round of kernel work to sample: system calls, so that the time it
/// takes is the kernel's, which the kernel target records.
auto kernel_work() -> void {
    for (auto i = 0; i < 10'000 / ROUNDS; ++i) {
        auto vmo = ours::Uandle::Invalid;
        if (ours::vmo_create(4096, ours::VmoOptions::None, &vmo) == ours::Status::Ok) {
            auto _ = ours::handle_close(vmo);
        }
    }
}

/// One round of user work to sample: arithmetic that never enters the
/// kernel, two calls deep, so that a user record has frames to walk. The
/// volatile store is what keeps the compiler from folding the loop away.
[[gnu::noinline]]
auto spin_inner(u64 const rounds) -> u64 {
    static u64 volatile SINK = 0;
    for (u64 i = 0; i < rounds; ++i) {
        SINK = SINK + i;
    }
    return SINK;
}

/// Not a tail call: a `jmp` to spin_inner would take this frame off the
/// stack, and with it the frame pointer that leads the walk to main.
[[gnu::noinline, clang::disable_tail_calls]]
auto user_work() -> u64 {
    return spin_inner(20'000'000 / ROUNDS);
}

/// Writes `PROF1 <hex of |bytes|>`: the header is 40 bytes, so its line is
/// 86, well inside one debug_write.
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
/// Base64, not the hex the header uses, because of the size: a record is 160
/// bytes, 320 hex digits, and one debug_write takes at most MAX_DEBUG_WRITE
/// (256) bytes. Splitting a line over two writes would let another CPU's
/// console output land in the middle of it. In base64 the record is 216
/// characters and the line 222, one write.
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

    auto const fetched = ours_resource::get(ours_resource::SAMPLING_PROTOCOL);
    if (fetched.is_err()) {
        report("getting the Sampling resource from /svc", fetched.unwrap_err());
        return 1;
    }
    auto const resource = fetched.unwrap();

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
    for (auto round = 0; round < ROUNDS; ++round) {
        kernel_work();
        auto _ = user_work();
    }
    if (auto const status = ours::sampler_stop(sampler); status != Status::Ok) {
        report("sampler_stop", status);
        return 1;
    }

    usize actual = 0;
    if (auto const status = ours::sampler_read(sampler, DUMP, sizeof(DUMP), &actual);
        status != Status::Ok) {
        report("sampler_read", status);
        return 1;
    }
    // The session's work is done; closing its handle ends it, so another
    // program may create one.
    auto _ = ours::handle_close(sampler);

    ours::SamplerDumpHeader header{};
    std::memcpy(&header, DUMP, sizeof(header));
    print_header(header);
    u32 user = 0;
    for (u32 i = 0; i < header.num_records; ++i) {
        ours::SamplerRecord record{};
        std::memcpy(&record, DUMP + header.header_size + usize{i} * header.record_size,
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
