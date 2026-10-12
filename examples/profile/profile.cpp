/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// profile: samples the kernel and every process while it runs a workload of
// system calls and of user-space arithmetic, and prints the samples for the
// host's prof-fold.
//
// It is the sampling profiler driven from user space, end to end: the
// Sampling resource, which it reaches at /svc/ours.resource.Sampling in the
// namespace it was started with, gives it a session (sampler_create),
// which it starts, keeps busy, stops and reads (sampler_start, _stop,
// _read). The dump goes to the console as one `PROF1 <hex>` line for the
// header and one `SAMPB <base64>` line per record:
//
//   prof-fold --elf vmours --samples console.txt
//             --debug-json out/<dir>/kernel.dev.debug.json > folded.txt
//
// Before the dump it says where the code of each process its user records
// name lies, so that their samples can be resolved: a `PROFP <koid>` line
// naming the process, which is the pid its user records carry, then its
// modules and segments in LLVM's symbolizer markup ({{{module:...}}},
// {{{mmap:...}}}), which give each module's build ID and where each segment
// was loaded. It learns them from outside each process (describe.hpp):
// the root job, from /svc/ours.kernel.RootJob, lists the processes there
// are, and a debugger channel bound to it tells of every one that starts
// while the session runs, before it runs.
//
// To have another process's code in the samples, it starts a copy of itself
// as a worker (argv[1] WORKER_ARGUMENT) that only does the user work and
// prints nothing: its frames are named only through what this program
// learned from outside it.
//
// Exits 0 when the session recorded both kernel and user samples, some of
// the user ones in this program's own code and some in the worker's; 1 when
// a resource could not be had or a call failed; 2 when a target recorded
// nothing, or no user sample lay in this program or in the worker.
#include <ours/exception.hpp>
#include <ours/object_info.hpp>
#include <ours/rights.hpp>
#include <ours/sampler.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <cstring>

#include "describe.hpp"
#include <eftl/elf/details/segment.hpp>
#include <fsio/fsio.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
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

/// |permissions| as the markup writes them: r, w and x, in that order, each
/// there or left out.
auto markup_flags(eftl::elf::SegPerms const permissions, char (&buffer)[3]) -> std::string_view {
    using eftl::elf::SegPerms;

    usize size = 0;
    for (auto const [permission, letter]:
         {std::pair{SegPerms::Readable, 'r'}, std::pair{SegPerms::Writable, 'w'},
          std::pair{SegPerms::Executable, 'x'}}) {
        if ((permissions & permission) != SegPerms::None) {
            buffer[size++] = letter;
        }
    }
    return {buffer, size};
}

/// argv[1] of the copy this program starts as its worker.
constexpr std::string_view WORKER_ARGUMENT = "--worker";

/// How many rounds of user work the worker does: twice this program's, so
/// that it is still at work for most of the session it was started for.
constexpr auto WORKER_ROUNDS = 2 * ROUNDS;

/// The most processes described, and the most koids a listing reads. Past
/// either, the rest are not described, and their samples stay addresses.
constexpr usize MAX_PROCESSES = 32;
constexpr usize MAX_LISTED = 128;

/// The processes described so far, by koid: those the root job listed when
/// the session was set up, and those that started since.
std::array<profile::Process, MAX_PROCESSES> PROCESSES{};
usize NUM_PROCESSES = 0;

auto find_process(ours::KoId const koid) -> profile::Process const * {
    auto const described = std::span{PROCESSES}.first(NUM_PROCESSES);
    auto const found = std::ranges::find_if(
        described, [koid](profile::Process const &process) { return process.koid == koid; });
    return found == described.end() ? nullptr : &*found;
}

/// Describes the process |handle| names, whose koid is |koid|, in place of
/// an earlier description of it: a process listed before it started maps
/// its code before it starts, and the later look is the one that saw it.
auto note_process(ours::Uandle const handle, ours::KoId const koid) -> void {
    auto const *const known = find_process(koid);
    auto *slot =
        known != nullptr ? &PROCESSES[static_cast<usize>(known - PROCESSES.data())] : nullptr;
    if (slot == nullptr) {
        if (NUM_PROCESSES == MAX_PROCESSES) {
            return;
        }
        slot = &PROCESSES[NUM_PROCESSES];
    }
    if (auto const status = profile::describe(handle, koid, slot); status != ours::Status::Ok) {
        report("describing a process", status);
        return;
    }
    if (known == nullptr) {
        ++NUM_PROCESSES;
    }
}

/// Describes every process |job| lists that is not described yet, through
/// a handle object_get_child() gives with the job handle's rights. One that
/// dies between the listing and the lookup (NotFound) is skipped.
auto describe_listed(ours::Uandle const job) -> ours::Status {
    static ours::KoId KOIDS[MAX_LISTED];
    usize actual = 0;
    if (auto const status = ours::object_get_info(job, ours::InfoTopic::JobProcesses, KOIDS,
                                                  sizeof(KOIDS), &actual, nullptr);
        status != ours::Status::Ok) {
        return status;
    }
    for (auto const koid: std::span{KOIDS}.first(actual)) {
        if (find_process(koid) != nullptr) {
            continue;
        }
        auto child = ours::Uandle::Invalid;
        if (ours::object_get_child(job, koid, ours::Rights::SameRights, &child) ==
            ours::Status::Ok) {
            note_process(child, koid);
            auto _ = ours::handle_close(child);
        }
    }
    return ours::Status::Ok;
}

/// Answers the ProcessStarting exceptions waiting on |debugger|, describing
/// each process first: its first thread waits in the kernel, before its
/// first instruction, until the exception's handle is closed. With |until|,
/// waits for the start of the process with that koid; with 0, takes only
/// what is there already. False, reported, when the channel failed.
auto answer_starts(ours::Uandle const debugger, ours::KoId const until) -> bool {
    using ours::Signals;
    using ours::Status;

    constexpr ours::Instant NOW = 0;
    for (;;) {
        auto seen = Signals::None;
        auto const waited = ours::object_wait_one(debugger, Signals::Readable | Signals::PeerClosed,
                                                  until == 0 ? NOW : ours::INSTANT_INFINITE, &seen);
        if (waited == Status::TimeOut) {
            return true;
        }
        if (waited != Status::Ok || !(seen & Signals::Readable)) {
            report("waiting on the debugger channel", waited);
            return false;
        }
        ours::ExceptionInfo info{};
        auto exception = ours::Uandle::Invalid;
        u32 bytes = 0;
        u32 handles = 0;
        if (auto const read = ours::channel_read(debugger, ours::ChannelReadOptions::None, &info,
                                                 &exception, sizeof(info), 1, &bytes, &handles);
            read != Status::Ok || bytes != sizeof(info) || handles != 1) {
            report("reading the debugger channel", read);
            return false;
        }
        auto process = ours::Uandle::Invalid;
        if (ours::exception_get_process(exception, &process) == Status::Ok) {
            note_process(process, info.pid);
            auto _ = ours::handle_close(process);
        }
        // Whatever it is closed with, the process goes on starting.
        auto _ = ours::handle_close(exception);
        if (until != 0 && info.pid == until) {
            return true;
        }
    }
}

/// Writes whose samples follow and where their code lies: `PROFP <koid>`,
/// then the process's modules in symbolizer markup -- a reset, then for
/// each module, numbered from 0, the module with its build ID and one mmap
/// line per loaded segment.
auto print_process(profile::Process const &process) -> void {
    static constexpr char DIGITS[] = "0123456789abcdef";

    char koid_text[21];
    auto _ =
        ort::debug_line({"PROFP ", ort::signed_decimal(static_cast<i64>(process.koid), koid_text)});
    auto _ = ort::debug_line("{{{reset}}}");
    for (usize index = 0; index < process.num_modules; ++index) {
        auto const &module = process.modules[index];
        char number[10];
        auto const id_text = ort::decimal(static_cast<u32>(index), number);
        char id[2 * profile::MAX_BUILD_ID];
        for (usize i = 0; i < module.build_id_size; ++i) {
            id[2 * i] = DIGITS[module.build_id[i] >> 4];
            id[2 * i + 1] = DIGITS[module.build_id[i] & 0xf];
        }
        auto const name = module.name_size != 0
                              ? std::string_view{module.name.data(), module.name_size}
                              : std::string_view{"unnamed"};
        auto _ = ort::debug_line(
            {"{{{module:", id_text, ":", name, ":elf:", {id, 2 * module.build_id_size}, "}}}"});
        for (auto const &segment: std::span{module.segments}.first(module.num_segments)) {
            char start[18];
            char size[18];
            char flags[3];
            char relative[18];
            auto _ = ort::debug_line({"{{{mmap:", ort::hex(segment.start, start), ":",
                                      ort::hex(segment.size, size), ":load:", id_text, ":",
                                      markup_flags(segment.permissions, flags), ":",
                                      ort::hex(segment.relative, relative), "}}}"});
        }
    }
}

/// The koid |handle| names, or 0 when it names nothing.
auto koid_of(ours::Uandle const handle) -> ours::KoId {
    ours::InfoHandleBasic basic{};
    if (ours::object_get_info(handle, ours::InfoTopic::HandleBasic, &basic, sizeof(basic), nullptr,
                              nullptr) != ours::Status::Ok) {
        return 0;
    }
    return basic.koid;
}

/// The worker: the user work and nothing else, not even a line of output,
/// so that whatever names its frames learned of its code from outside it.
auto work() -> int {
    for (auto round = 0; round < WORKER_ROUNDS; ++round) {
        auto _ = user_work();
    }
    return 0;
}

/// The processes there are and the worker, described before the session
/// starts: |debugger| is bound to |root_job| first, so a process that starts
/// while the listing runs is told of on it, and none falls between the two.
/// The worker waits in the kernel, before its first instruction, until its
/// start is answered here. Its process goes to |worker|.
auto set_up(ours::Uandle const root_job, ours::Uandle const debugger, std::string_view const self,
            ours::Uandle *const worker) -> bool {
    if (auto const status = describe_listed(root_job); status != ours::Status::Ok) {
        report("object_get_info(the root job, JobProcesses)", status);
        return false;
    }
    std::string_view const argv[] = {self, WORKER_ARGUMENT};
    auto const spawned = fsio::spawn(root_job, self, argv);
    if (spawned.is_err()) {
        report("starting the worker", spawned.unwrap_err());
        return false;
    }
    *worker = spawned.unwrap();
    return answer_starts(debugger, koid_of(*worker));
}
} // namespace

auto main(int argc, char **argv) -> int {
    using ours::Status;

    if (argc >= 2 && argv[1] != nullptr && std::string_view{argv[1]} == WORKER_ARGUMENT) {
        return work();
    }
    if (argc < 1 || argv[0] == nullptr) {
        auto _ = ort::debug_line("profile: no argv[0] to start the worker from");
        return 1;
    }

    auto const fetched = ours_resource::get(ours_resource::SAMPLING_PROTOCOL);
    if (fetched.is_err()) {
        report("getting the Sampling resource from /svc", fetched.unwrap_err());
        return 1;
    }
    auto const resource = fetched.unwrap();
    auto const root_fetched = ours_resource::get(ours_resource::ROOT_JOB_PROTOCOL);
    if (root_fetched.is_err()) {
        report("getting the root job from /svc", root_fetched.unwrap_err());
        return 1;
    }
    auto const root_job = root_fetched.unwrap();
    auto debugger = ours::Uandle::Invalid;
    if (auto const status = ours::task_create_exception_channel(
            root_job, ours::ExceptionChannelOptions::Debugger, &debugger);
        status != Status::Ok) {
        report("task_create_exception_channel(the root job, Debugger)", status);
        return 1;
    }
    auto worker = ours::Uandle::Invalid;
    if (!set_up(root_job, debugger, argv[0], &worker)) {
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
    // Between rounds, the starts of processes something else began: each
    // waits until it is answered, so not for longer than a round.
    for (auto round = 0; round < ROUNDS; ++round) {
        kernel_work();
        auto _ = user_work();
        if (!answer_starts(debugger, 0)) {
            return 1;
        }
    }
    // The worker's rounds are in the session too.
    if (auto const status = ours::object_wait_one(worker, ours::Signals::Terminated,
                                                  ours::INSTANT_INFINITE, nullptr);
        status != Status::Ok) {
        report("waiting for the worker", status);
        return 1;
    }
    if (!answer_starts(debugger, 0)) {
        return 1;
    }
    if (auto const status = ours::sampler_stop(sampler); status != Status::Ok) {
        report("sampler_stop", status);
        return 1;
    }
    // Nothing waits on it from here on: closing it unbinds it, and a start
    // sent to it after is not held.
    auto _ = ours::handle_close(debugger);

    usize actual = 0;
    if (auto const status = ours::sampler_read(sampler, DUMP, sizeof(DUMP), &actual);
        status != Status::Ok) {
        report("sampler_read", status);
        return 1;
    }
    // The session's work is done; closing its handle ends it, so another
    // program may create one.
    auto _ = ours::handle_close(sampler);

    auto const self = koid_of(ort::process_self());
    auto const worker_koid = koid_of(worker);
    auto _ = ours::handle_close(worker);
    auto _ = ours::handle_close(root_job);
    auto const *const described_self = find_process(self);
    auto const *const described_worker = find_process(worker_koid);
    if (described_self == nullptr || described_worker == nullptr) {
        auto _ = ort::debug_line("profile: this program or its worker was not described");
        return 1;
    }

    ours::SamplerDumpHeader header{};
    std::memcpy(&header, DUMP, sizeof(header));
    auto const record_at = [&header](u32 const index) {
        ours::SamplerRecord record{};
        std::memcpy(&record, DUMP + header.header_size + usize{index} * header.record_size,
                    sizeof(record));
        return record;
    };

    // Each described process that a user record names, once.
    u32 named = 0;
    for (auto const &process: std::span{PROCESSES}.first(NUM_PROCESSES)) {
        for (u32 i = 0; i < header.num_records; ++i) {
            auto const record = record_at(i);
            if (!!(record.flags & ours::SamplerRecordFlags::FromUser) &&
                record.pid == process.koid) {
                print_process(process);
                ++named;
                break;
            }
        }
    }

    print_header(header);
    u32 user = 0;
    u32 own = 0;
    u32 worker_samples = 0;
    for (u32 i = 0; i < header.num_records; ++i) {
        auto const record = record_at(i);
        if (!!(record.flags & ours::SamplerRecordFlags::FromUser)) {
            ++user;
            // A pc a described module holds says the markup above put the
            // code where the samples found it.
            if (record.pid == self && described_self->runs(record.ips[0])) {
                ++own;
            }
            if (record.pid == worker_koid && described_worker->runs(record.ips[0])) {
                ++worker_samples;
            }
        }
        print_record(record);
    }

    char kernel_count[10];
    char user_count[10];
    char own_count[10];
    char worker_count[10];
    char dropped[10];
    auto _ = ort::debug_line({"profile: ", ort::decimal(header.num_records - user, kernel_count),
                              " kernel samples, ", ort::decimal(user, user_count),
                              " user samples (", ort::decimal(own, own_count), " in this program, ",
                              ort::decimal(worker_samples, worker_count), " in its worker), ",
                              ort::decimal(header.num_dropped, dropped), " dropped"});
    char described_count[10];
    char named_count[10];
    auto _ = ort::debug_line(
        {"profile: ", ort::decimal(static_cast<u32>(NUM_PROCESSES), described_count),
         " processes described, ", ort::decimal(named, named_count), " named in the samples"});
    if (user == 0 || user == header.num_records || own == 0 || worker_samples == 0) {
        return 2;
    }
    return 0;
}
