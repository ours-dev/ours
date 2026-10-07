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
// Before the dump it says where its own code lies, so that its user samples
// can be resolved: a `PROFP <koid>` line naming the process, which is the pid
// its user records carry, then this program's module and segments in LLVM's
// symbolizer markup ({{{module:...}}}, {{{mmap:...}}}), which gives its build
// ID and where each segment was loaded.
//
// Exits 0 when the session recorded both kernel and user samples, and some
// of the user ones in this program's own code; 1 when the resource could not
// be had or a call failed; 2 when a target recorded nothing, or no user
// sample lay in this program.
#include <ours/object_info.hpp>
#include <ours/sampler.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <cstring>

#include <eftl/binary_stream.hpp>
#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/details/structures.hpp>
#include <eftl/elf/note.hpp>
#include <eftl/elf/traits.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>
#include <ustl/option.hpp>

/// This program's ELF header where it lies in memory: the linker defines the
/// symbol at the first byte of the image, so its address is where the image
/// was loaded, and the program headers follow it in the same loaded page.
/// Hidden, so that the reference is resolved when the program is linked: the
/// loaders' binder binds only to the vDSO, and could not bind a dynamic one.
// The name is the linker's, so neither the reserved prefix nor the case is
// this program's to choose.
// NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming)
extern "C" [[gnu::visibility("hidden")]]
eftl::elf::Elf64Ehdr const __ehdr_start;
// NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming)

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

/// The unit the loader maps a segment in: whole pages.
constexpr u64 LOAD_PAGE = 4096;

/// The type of the note a linker's --build-id writes, under the owner name
/// "GNU" (NT_GNU_BUILD_ID).
constexpr u32 NOTE_GNU_BUILD_ID = 3;

/// The longest build ID this prints: a SHA-256 is 32 bytes, and the build
/// here asks for SHA-1, 20 (//build/config:build_id).
constexpr usize MAX_BUILD_ID = 32;

/// One of this program's loaded segments, in whole pages: where it lies in
/// memory, how long it is, its rights, and where it lies in the ELF's own
/// addresses, which is what a symbolizer looks an address up by.
struct LoadedSegment {
    u64 start;
    u64 size;
    eftl::elf::SegPerms permissions;
    u64 relative;
};

/// What the markup says about this program.
struct Module {
    std::span<u8 const> build_id;
    std::array<LoadedSegment, 8> segments{};
    usize num_segments = 0;

    /// Whether |address| lies in one of the executable segments: where a
    /// sample of this program's own code has its pc.
    [[nodiscard]]
    auto runs(u64 const address) const -> bool {
        return std::ranges::any_of(
            std::span{segments}.first(num_segments), [address](LoadedSegment const &segment) {
                return (segment.permissions & eftl::elf::SegPerms::Executable) !=
                           eftl::elf::SegPerms::None &&
                       address - segment.start < segment.size;
            });
    }
};

/// Reads this program's module from its own headers, where they lie in
/// memory. None when they do not say where the image was loaded, or carry no
/// build ID, which every program linked here has.
auto read_module() -> ustl::Option<Module> {
    using eftl::elf::Elf64Phdr;
    using eftl::elf::SegType;

    auto const image = reinterpret_cast<u64>(&__ehdr_start);
    auto const headers = std::span{
        reinterpret_cast<Elf64Phdr const *>(image + __ehdr_start.e_phoff), __ehdr_start.e_phnum};

    // The segment that holds file offset 0, the ELF header, says which
    // program address the header was linked at. The header's address in
    // memory less that is the bias every program address is offset by.
    auto const first = std::ranges::find_if(headers, [](Elf64Phdr const &header) {
        return header.type() == SegType::Load && header.p_offset == 0;
    });
    if (first == headers.end()) {
        return ustl::NONE;
    }
    auto const bias = image - first->p_vaddr;

    Module module{};
    for (auto const &header: headers) {
        if (header.type() == SegType::Load && module.num_segments < module.segments.size()) {
            auto const begin = header.p_vaddr & ~(LOAD_PAGE - 1);
            auto const end = (header.p_vaddr + header.p_memsz + LOAD_PAGE - 1) & ~(LOAD_PAGE - 1);
            module.segments[module.num_segments++] = LoadedSegment{
                .start = bias + begin,
                .size = end - begin,
                .permissions = header.permissions(),
                .relative = begin,
            };
            continue;
        }
        if (header.type() != SegType::Note) {
            continue;
        }
        // A note segment is loaded with the rest, so it is read in place.
        auto const notes = eftl::elf::NoteSectionView<eftl::elf::Traits64>::from(
            eftl::ByteRange(reinterpret_cast<u8 const *>(bias + header.p_vaddr), header.p_filesz));
        if (notes.is_none()) {
            continue;
        }
        for (usize index = 0; index < notes->size(); ++index) {
            auto const note = notes->note_at(index);
            if (note.is_none() || note->m_type != NOTE_GNU_BUILD_ID) {
                continue;
            }
            auto const owner = std::string_view{reinterpret_cast<char const *>(note->m_name.data()),
                                                note->m_name.size()};
            if (owner == std::string_view{"GNU", 4}) {
                module.build_id = {note->m_descriptor.data(), note->m_descriptor.size()};
            }
        }
    }
    if (module.build_id.empty() || module.build_id.size() > MAX_BUILD_ID) {
        return ustl::NONE;
    }
    return ustl::some(module);
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

/// Writes whose samples follow and where their code lies: `PROFP <koid>`,
/// then |module| in symbolizer markup -- a reset, the module with its build
/// ID, and one mmap line per loaded segment. Module 0 throughout: a program
/// here is one ELF. The vDSO, the one other image mapped into it, is not
/// described, so a sample in it stays a bare address.
auto print_module(ours::KoId const koid, std::string_view const name, Module const &module)
    -> void {
    static constexpr char DIGITS[] = "0123456789abcdef";

    char koid_text[21];
    auto _ = ort::debug_line({"PROFP ", ort::signed_decimal(static_cast<i64>(koid), koid_text)});
    auto _ = ort::debug_line("{{{reset}}}");
    char id[2 * MAX_BUILD_ID];
    for (usize i = 0; i < module.build_id.size(); ++i) {
        id[2 * i] = DIGITS[module.build_id[i] >> 4];
        id[2 * i + 1] = DIGITS[module.build_id[i] & 0xf];
    }
    auto _ =
        ort::debug_line({"{{{module:0:", name, ":elf:", {id, 2 * module.build_id.size()}, "}}}"});
    for (auto const &segment: std::span{module.segments}.first(module.num_segments)) {
        char start[18];
        char size[18];
        char flags[3];
        char relative[18];
        auto _ = ort::debug_line({"{{{mmap:", ort::hex(segment.start, start), ":",
                                  ort::hex(segment.size, size),
                                  ":load:0:", markup_flags(segment.permissions, flags), ":",
                                  ort::hex(segment.relative, relative), "}}}"});
    }
}

/// The last part of argv[0], the program's name in the markup, which may
/// hold neither a ':' nor a '}'; "profile" when argv is empty.
auto program_name(int const argc, char **const argv) -> std::string_view {
    if (argc < 1 || argv[0] == nullptr) {
        return "profile";
    }
    auto const path = std::string_view{argv[0]};
    return path.substr(path.rfind('/') + 1);
}
} // namespace

auto main(int argc, char **argv) -> int {
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

    // Whose user samples are whose: this process's koid, which its user
    // records carry as pid, and where its code was loaded.
    ours::InfoHandleBasic self{};
    if (auto const status = ours::object_get_info(ort::process_self(), ours::InfoTopic::HandleBasic,
                                                  &self, sizeof(self), nullptr, nullptr);
        status != Status::Ok) {
        report("object_get_info(process_self(), HandleBasic)", status);
        return 1;
    }
    auto const module = read_module();
    if (module.is_none()) {
        auto _ = ort::debug_line("profile: its headers give no load address or no build ID");
        return 1;
    }
    print_module(self.koid, program_name(argc, argv), *module);

    ours::SamplerDumpHeader header{};
    std::memcpy(&header, DUMP, sizeof(header));
    print_header(header);
    u32 user = 0;
    u32 own = 0;
    for (u32 i = 0; i < header.num_records; ++i) {
        ours::SamplerRecord record{};
        std::memcpy(&record, DUMP + header.header_size + usize{i} * header.record_size,
                    sizeof(record));
        if (!!(record.flags & ours::SamplerRecordFlags::FromUser)) {
            ++user;
            // A pc this process's own code holds says the markup above put
            // the code where the samples found it.
            if (record.pid == self.koid && module->runs(record.ips[0])) {
                ++own;
            }
        }
        print_record(record);
    }

    char kernel_count[10];
    char user_count[10];
    char own_count[10];
    char dropped[10];
    auto _ = ort::debug_line(
        {"profile: ", ort::decimal(header.num_records - user, kernel_count), " kernel samples, ",
         ort::decimal(user, user_count), " user samples (", ort::decimal(own, own_count),
         " in this program), ", ort::decimal(header.num_dropped, dropped), " dropped"});
    if (user == 0 || user == header.num_records || own == 0) {
        return 2;
    }
    return 0;
}
