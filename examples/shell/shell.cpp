/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// A user-mode command shell that talks to the kernel only through the vDSO.
///
/// It provides a prompt, bounded line editing, built-ins, and foreground
/// launching of programs. The syscall checks below still run before the
/// prompt, so boot verification exercises the same kernel boundary.
///
/// Its system calls are the vDSO's entries, ours_* from <ours/vdso.hpp>,
/// called directly, the checks of the raw ABI (bad pointers, option words)
/// among them. It knows nothing else about the boundary -- not the symbols
/// the loader binds, not that a trap is involved at all.
///
/// It runs on the runtime (//system/lib/ort): the entry point is the
/// runtime's, which reads the processargs message this program was started
/// with and calls main. What main finds among the startup handles says who
/// started it and how it starts programs (component-framework design,
/// "Events: the shell running a program" and "Events: dev.dbg_1"):
///
///   - on dev, the root (init) starts it as a component of meta/root.cm,
///     from its manifest, shell.cm: a namespace with /boot and /svc, which
///     fsio binds; a duplicate of the root job; the vDSO's VMO and its root
///     region. It runs a command by spawning /boot/bin/<name> through the
///     root's ours.process.Launcher (fsio::spawn(), design D12), and asks
///     /svc's ours.resource.Debug for the System Debug resource it reads
///     serial input with (D19);
///   - on dev.dbg_1, userboot starts it directly, with no root: the root
///     job, the BootFS, VMEX, the vDSO's VMO, the Debug resource and its root
///     region. It starts BootFS programs with the launch library itself.
///
/// The C library it links is mem* and strlen (//system/lib/libc), and
/// check_libc() is where this program makes sure it is there and right.

#include <ours/object_info.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>

#include <cstring>

#include "programs.hpp"
#include "shell_launcher.hpp"
#include "shell_tests.hpp"
#include <bootfs/format.hpp>
#include <bootfs/view.hpp>
#include <fsio/fsio.hpp>
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <launch/program.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_io/ours_io.hpp>
#include <ours_resource/ours_resource.hpp>
#include <wire/wire.hpp>

namespace {
using shell::Launcher;
using shell::spells;
using shell::Starter;

/// Where the root binds the BootFS in this shell's namespace on dev
/// (shell.cm's use of `boot`), and so where its programs are: /boot/bin.
constexpr std::string_view BOOT_PATH{"/boot"};
constexpr std::string_view BOOT_PREFIX{"/boot/"};

/// The protocol the Debug resource comes through on dev (design D19).
constexpr std::string_view DEBUG_RESOURCE_PATH{"/svc/ours.resource.Debug"};

/// The kshell command registry, fetched into this program's own memory.
///
/// Sized rather than grown: there is no allocator in this image. NoSpace comes
/// back when the registry outgrows it, and the part that fits is still shown
/// -- a truncated list beats no list when the point is to see what exists.
class Registry {
  public:
    auto fetch() -> ours::Status {
        return ours::command_list(m_text, sizeof(m_text));
    }

    /// Up to the first NUL, or the whole buffer if there is none. Bounded,
    /// because the kernel wrote it: it promises a terminator, and this does
    /// not depend on the promise.
    [[nodiscard]]
    auto text() const -> std::string_view {
        auto const end = std::find(std::begin(m_text), std::end(m_text), '\0');
        return {m_text, static_cast<usize>(end - std::begin(m_text))};
    }

  private:
    char m_text[2048] = {};
};

constexpr std::string_view BANNER{"ours shell: user-mode serial command shell"};

/// Prints "<what><status name>" on a line of its own.
auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
}

/// Whether the namespace the runtime handed this process, which fsio::install()
/// bound, has a directory at exactly |path|.
auto is_bound(std::string_view const path) -> bool {
    auto const &installed = fsio::installed_namespace();
    for (usize index = 0; index < installed.size(); ++index) {
        if (auto const entry = installed.entry(index); entry.is_some() && entry->path == path) {
            return true;
        }
    }
    return false;
}

/// Debug.Get on |connection|, a new connection to ours.resource.Debug: the
/// resource, which the caller owns, or why not -- the reply's status, the
/// epitaph a failed route closed the connection with, PeerClosed when it
/// closed without one, or InvArgs for an answer that is not a well-formed
/// reply.
auto get_debug_resource(ours::Uandle const connection) -> ustl::Result<ours::Uandle, ours::Status> {
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    // The one request on this connection, so any txid will do.
    constexpr auto TXID = u32{1};
    std::array<u8, wire::STATUS_FORM_SIZE> request_bytes{};
    auto const request = ours_resource::encode_debug_get(request_bytes, {.txid = TXID});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    // A write that finds the connection closed is not the answer: the
    // epitaph the root closed it with is, and it is still there to read.
    if (auto const written =
            ours::channel_write(connection, ours::ChannelWriteOptions::None, request->bytes.data(),
                                static_cast<u32>(request->bytes.size()), nullptr, 0);
        written != Status::Ok && written != Status::PeerClosed) {
        return ustl::err(written);
    }
    auto observed = Signals::None;
    if (auto const status = ours::object_wait_one(
            connection, Signals::Readable | Signals::PeerClosed, ours::INSTANT_INFINITE, &observed);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (!(observed & Signals::Readable)) {
        return ustl::err(Status::PeerClosed);
    }

    // Either answer is the 24-byte status form or the reply, as long, with
    // at most the resource in it.
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    std::array<Uandle, 1> handles{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const status = ours::channel_read(connection, ours::ChannelReadOptions::None,
                                               bytes.data(), handles.data(), bytes.size(),
                                               handles.size(), &num_bytes, &num_handles);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const message = wire::Message{
        .bytes = std::span{bytes}.first(num_bytes),
        .handles = std::span{handles}.first(num_handles),
    };
    auto const close_handles = [&message] {
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
    };
    if (auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        header.is_ok() && wire::is_epitaph(header.unwrap())) {
        close_handles();
        auto const epitaph = wire::decode_epitaph(message);
        if (epitaph.is_err()) {
            return ustl::err(Status::InvArgs);
        }
        // An Ok epitaph says nothing about why; it reads as the connection
        // closing, as fsio reads one.
        if (epitaph.unwrap() == Status::Ok) {
            return ustl::err(Status::PeerClosed);
        }
        return ustl::err(epitaph.unwrap());
    }
    auto const decoded = ours_resource::decode_debug_get_reply(message, TXID);
    if (decoded.is_err()) {
        close_handles();
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return ustl::ok(decoded->resource);
}

/// The System Debug resource, which debug_read needs to read COM1, from
/// /svc/ours.resource.Debug: on dev the root hands it to no one at start,
/// only through that built-in protocol (design D19). One Get on a connection
/// of its own, closed after it.
auto debug_resource_from_svc() -> ustl::Result<ours::Uandle, ours::Status> {
    auto client = ours::Uandle::Invalid;
    auto server = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != ours::Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = fsio::connect(DEBUG_RESOURCE_PATH, server);
        status != ours::Status::Ok) {
        auto _ = ours::handle_close(client);
        return ustl::err(status);
    }
    auto const resource = get_debug_resource(client);
    auto _ = ours::handle_close(client);
    return resource;
}

/// Reads the BootFS header through vmo_read and says how many files it
/// holds, as init does (the design's check for this step, K14). Only the
/// shell userboot starts has a BootFS; on dev the root keeps it, and serves
/// it as /boot.
auto describe_bootfs(ours::Uandle const bootfs) -> void {
    if (bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: started without a BootFS");
        return;
    }
    bootfs::Header header{};
    if (auto const status = ours::vmo_read(bootfs, &header, 0, sizeof(header));
        status != ours::Status::Ok) {
        report("shell: reading the BootFS header = ", status);
        return;
    }
    if (header.magic != bootfs::Header::MAGIC) {
        auto _ = ort::debug_line("shell: the BootFS header has the wrong magic");
        return;
    }
    char count[10];
    auto _ =
        ort::debug_line({"shell: bootfs has ", ort::decimal(header.file_count, count), " entries"});
}

/// The `ls` built-in on dbg_1: every file's flat BootFS name, from the
/// BootFS mapping in Starter::bootfs_file -- the library's where the shell
/// has one, its own read-only map_bootfs() otherwise -- checked by
/// bootfs::View on the way.
auto list_bootfs(std::span<u8 const> const bootfs_file) -> ours::Status {
    auto const view = bootfs::View::from({bootfs_file.data(), bootfs_file.size()});
    if (view.is_err()) {
        return view.unwrap_err();
    }
    for (u32 index = 0; index < view.unwrap().file_count(); ++index) {
        auto _ = ort::debug_line(view.unwrap().file_name(index));
    }
    return ours::Status::Ok;
}

/// The `ls` built-in on dev (design D29): the entries of one directory of
/// the namespace, |path|, through Directory.ReadDirents, one name a line, a
/// directory's with a '/' after it. "." is left out, as ls leaves it out
/// without -a. |path| is absolute: there is no working directory, so fsio
/// answers NotFound for any other.
auto list_directory(std::string_view const path) -> ours::Status {
    using ours::Status;
    using ours_io::Flags;

    auto const opened = fsio::open(path, Flags::ProtocolDirectory | Flags::PermEnumerate);
    if (opened.is_err()) {
        return opened.unwrap_err();
    }
    auto const directory = opened.unwrap();
    auto status = Status::Ok;
    std::array<u8, ours_io::MAX_BUF> buffer{};
    for (auto done = false; !done;) {
        auto const read = fsio::read_dirents(directory, buffer);
        if (read.is_err()) {
            status = read.unwrap_err();
            break;
        }
        // Every entry has been read once a call fills nothing.
        done = read.unwrap() == 0;
        auto reader = ours_io::DirentReader(std::span{buffer}.first(read.unwrap()));
        for (auto next = reader.next(); !done; next = reader.next()) {
            if (next.is_err()) {
                status = next.unwrap_err();
                done = true;
                break;
            }
            if (next->is_none()) {
                break;
            }
            auto const &dirent = next->value();
            if (dirent.name == ".") {
                continue;
            }
            std::string_view suffix;
            if (dirent.type == ours_io::DirentType::Directory) {
                suffix = "/";
            }
            auto _ = ort::debug_line({dirent.name, suffix});
        }
    }
    // Close's answer adds nothing to the listing's; the handle goes either
    // way.
    auto _ = fsio::close(directory);
    return status;
}

/// The launcher's reports, on the debug console: why it refused or failed,
/// and where it bound each of the program's imports. The bound values are
/// what a fault through a wrongly bound slot needs beside its register dump.
/// The kernel logs them for the programs it starts; for the launcher's,
/// nothing else would.
auto print_launch_report(void *, elfload::Report const &report) -> void {
    std::string_view prefix{"shell: launch: "};
    if (report.severity == elfload::Severity::Error) {
        prefix = "shell: launch failed: ";
    }
    std::string_view separator;
    std::string_view name;
    if (!report.name.empty()) {
        separator = ": ";
        name = {report.name.data(), report.name.size()};
    }
    char text[18];
    std::string_view space;
    std::string_view value;
    if (report.value.is_some()) {
        space = " ";
        value = ort::hex(report.value.value(), text);
    }
    auto _ = ort::debug_line(
        {prefix, {report.what.data(), report.what.size()}, separator, name, space, value});
}

/// The launcher (launch::make_bootfs_launcher()): the BootFS mapped and made
/// executable with vmex, and the vDSO found in it. An invalid `executable`
/// means program launching is unavailable, while the shell's built-ins
/// remain usable. The library says which step failed (print_launch_report());
/// this adds the verdict K23's check reads.
auto make_launcher(ours::Uandle const bootfs, ours::Uandle const vmex, ours::Uandle const vdso,
                   ours::Uandle const vma) -> Launcher {
    if (bootfs == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid ||
        vdso == ours::Uandle::Invalid || vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line(
            "shell: launcher checks FAILED (missing BootFS, vmex, vDSO or VmaRoot)");
        return {};
    }
    auto made = launch::make_bootfs_launcher(bootfs, vmex, vdso, vma,
                                             {.report = print_launch_report, .context = nullptr});
    if (made.is_err()) {
        report("shell: launcher checks FAILED, setting the launcher up = ", made.unwrap_err());
        return {};
    }
    return made.unwrap();
}

/// Starts |name| from the BootFS under |job| with the launcher, |bootstrap|
/// as its one handle, consumed whatever the answer
/// (launch::launch_from_bootfs()).
auto launch_program(Launcher const &launcher, ours::Uandle const job, std::string_view const name,
                    ours::Uandle const bootstrap) -> ustl::Result<launch::Started, ours::Status> {
    return launch::launch_from_bootfs(launcher, job, name, bootstrap,
                                      {.report = print_launch_report, .context = nullptr});
}

auto launch_for_tests(void *const context, ours::Uandle const job, std::string_view const name,
                      ours::Uandle const bootstrap) -> ustl::Result<launch::Started, ours::Status> {
    auto const *const launcher = static_cast<Launcher const *>(context);
    return launch_program(*launcher, job, name, bootstrap);
}

constexpr usize SHELL_LINE_BYTES = 128;
constexpr usize SHELL_ARG_MAX = 16;

/// Room for any command's path: /boot/ on dev, the directory, a `/`, and a
/// word of a line.
using CommandPath =
    char[BOOT_PREFIX.size() + user_programs::DIRECTORY.size() + 1 + SHELL_LINE_BYTES];

/// The path |command| runs from, built in |path| when it is not |command|
/// itself: a BootFS name on dbg_1, where |root| is empty, and a namespace
/// path under |root|, "/boot/", on dev.
///
/// A command with a `/` in it is a path and is taken as typed: on dev, an
/// absolute path in the namespace (design D29). Any other is looked up in the
/// directory programs install to (user_programs::DIRECTORY, "bin"), as a Unix
/// shell finds it on a PATH of one entry: `shell` runs bin/shell,
/// /boot/bin/shell on dev. Nothing else is searched. The other top-level files
/// are the vDSO, manifests and configuration, which no command names, and a
/// fallback would only turn a mistyped program name into an attempt to run one
/// of them.
auto command_path(std::string_view const root, std::string_view const command, CommandPath &path)
    -> std::string_view {
    if (command.find('/') != std::string_view::npos) {
        return command;
    }
    auto const directory = user_programs::DIRECTORY;
    auto const size = root.size() + directory.size() + 1 + command.size();
    if (size > sizeof(path)) {
        return {};
    }
    std::memcpy(path, root.data(), root.size());
    std::memcpy(path + root.size(), directory.data(), directory.size());
    path[root.size() + directory.size()] = '/';
    std::memcpy(path + root.size() + directory.size() + 1, command.data(), command.size());
    return {path, size};
}

/// Starts |path| under |job| with the library, |args| as its argv: the
/// launch, then the processargs on the bootstrap channel with the program's
/// root region. Failures are reported here. dbg_1's way.
auto start_with_library(Launcher const &launcher, ours::Uandle const job,
                        std::string_view const path, std::span<std::string_view const> const args)
    -> ustl::Result<ours::Uandle, ours::Status> {
    using ours::Status;

    ours::Uandle bootstrap = ours::Uandle::Invalid;
    ours::Uandle bootstrap_there = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &bootstrap, &bootstrap_there);
        status != Status::Ok) {
        report("shell: launch channel_create = ", status);
        return ustl::err(status);
    }

    auto const started = launch_program(launcher, job, path, bootstrap_there);
    if (started.is_err()) {
        auto _ = ours::handle_close(bootstrap);
        return ustl::err(started.unwrap_err());
    }
    auto const child = started.unwrap();

    u8 message[512];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::VmaRoot, 0)};
    if (auto const status = ours::encode_process_args(message, infos, args, {}, {}, &length);
        status != Status::Ok) {
        report("shell: encoding program processargs = ", status);
        auto _ = ours::handle_close(bootstrap);
        auto _ = ours::handle_close(child.root_vma);
        auto _ = ours::handle_close(child.process);
        return ustl::err(status);
    }

    auto root_vma = child.root_vma;
    auto const written = ours::channel_write(bootstrap, ours::ChannelWriteOptions::None, message,
                                             length, &root_vma, 1);
    auto _ = ours::handle_close(bootstrap);
    if (written != Status::Ok) {
        report("shell: writing program processargs = ", written);
        auto _ = ours::handle_close(child.process);
        return ustl::err(written);
    }
    return ustl::ok(child.process);
}

/// Runs a typed command and waits for it to end: on dev, spawned through the
/// root's ours.process.Launcher with a clone of this shell's namespace, with
/// fsio::spawn() (design D12); on dbg_1, started with the library. A command
/// that ends with a code other than 0 says so.
auto launch_command(Starter const &starter, ours::Uandle const job,
                    std::span<std::string_view const> const args) -> void {
    using ours::Signals;
    using ours::Status;

    // argv[0] stays as typed, as a Unix shell leaves it; only the lookup
    // takes the full name.
    CommandPath path_buffer;
    std::string_view root;
    if (starter.component) {
        root = BOOT_PREFIX;
    }
    auto const path = command_path(root, args[0], path_buffer);
    if (path.empty()) {
        auto _ = ort::debug_line("shell: command name too long");
        return;
    }

    auto process = ours::Uandle::Invalid;
    if (starter.component) {
        // The Launcher adds the root region and the vDSO; the command gets
        // argv, a duplicate of |job| and this shell's namespace, cloned.
        auto const spawned = fsio::spawn(job, path, args);
        if (spawned.is_err()) {
            report("shell: launch failed: ", spawned.unwrap_err());
            return;
        }
        process = spawned.unwrap();
    } else {
        auto const started = start_with_library(starter.library, job, path, args);
        if (started.is_err()) {
            return;
        }
        process = started.unwrap();
    }

    auto observed = Signals::None;
    if (auto const status =
            ours::object_wait_one(process, Signals::Terminated, ours::INSTANT_INFINITE, &observed);
        status != Status::Ok) {
        report("shell: waiting for program = ", status);
    } else {
        ours::InfoProcess info{};
        if (auto const status = ours::object_get_info(process, ours::InfoTopic::Process, &info,
                                                      sizeof(info), nullptr, nullptr);
            status != Status::Ok) {
            report("shell: reading the program's exit code = ", status);
        } else if (info.return_code != 0) {
            char code[21];
            auto _ = ort::debug_line(
                {"shell: ", args[0], " exited with ", ort::signed_decimal(info.return_code, code)});
        }
    }
    auto _ = ours::handle_close(process);
}

auto split_words(char *const line, std::string_view *const words, usize const capacity) -> usize {
    auto *cursor = line;
    usize count = 0;
    while (*cursor != '\0' && count < capacity) {
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        auto *const start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            ++cursor;
        }
        words[count++] = {start, static_cast<usize>(cursor - start)};
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
    }
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor != '\0') {
        return capacity + 1;
    }
    return count;
}

auto dispatch_command(Starter const &starter, ours::Uandle const job, std::string_view *const args,
                      usize const count) -> bool {
    if (count == 0) {
        return false;
    }
    if (args[0] == "exit") {
        return true;
    }
    if (args[0] == "help") {
        if (starter.component) {
            auto _ = ort::debug_line("shell: builtins: help ls [directory] echo commands clear "
                                     "exit; other names run /boot/bin programs");
        } else {
            auto _ = ort::debug_line("shell: builtins: help ls echo commands clear exit; other "
                                     "names run BootFS programs");
        }
        return false;
    }
    if (args[0] == "clear") {
        auto _ = ort::debug_write("\x1b[2J\x1b[H");
        return false;
    }
    if (args[0] == "ls") {
        // dbg_1 keeps its flat BootFS names (D29); dev lists one directory
        // of its namespace, /boot unless another is named.
        if (!starter.component) {
            if (auto const status = list_bootfs(starter.bootfs_file); status != ours::Status::Ok) {
                report("shell: listing BootFS = ", status);
            }
            return false;
        }
        if (count > 2) {
            auto _ = ort::debug_line("shell: usage: ls [directory]");
            return false;
        }
        auto path = BOOT_PATH;
        if (count == 2) {
            path = args[1];
        }
        if (auto const status = list_directory(path); status != ours::Status::Ok) {
            auto _ = ort::debug_line(
                {"shell: ls ", path, ": ", std::string_view{ours::to_string(status)}});
        }
        return false;
    }
    if (args[0] == "echo") {
        char output[SHELL_LINE_BYTES] = {};
        usize output_size = 0;
        for (usize index = 1; index < count; ++index) {
            if (index > 1) {
                output[output_size++] = ' ';
            }
            for (auto const byte: args[index]) {
                output[output_size++] = byte;
            }
        }
        auto _ = ort::debug_line(std::string_view{output, output_size});
        return false;
    }
    if (args[0] == "commands") {
        static Registry registry;
        auto const status = registry.fetch();
        auto _ = ort::debug_line("--- kernel commands ---");
        auto _ = ort::debug_write(registry.text());
        if (status != ours::Status::Ok && status != ours::Status::NoSpace) {
            report("shell: command_list = ", status);
        }
        return false;
    }
    if (job == ours::Uandle::Invalid || !starter.can_start()) {
        auto _ = ort::debug_line("shell: program launching is unavailable");
        return false;
    }
    launch_command(starter, job, {args, count});
    return false;
}

auto interactive_shell(ours::Uandle const debug_resource, ours::Uandle const job,
                       Starter const &starter) -> int {
    using ours::Status;

    constexpr std::string_view PROMPT{"ours> "};
    char line[SHELL_LINE_BYTES] = {};
    char input[ours::MAX_DEBUG_READ] = {};
    usize line_size = 0;
    auto line_overflow = false;
    auto _ = ort::debug_write(PROMPT);

    for (;;) {
        usize actual = 0;
        if (auto const status = ours::debug_read(debug_resource, input, sizeof(input), &actual);
            status != Status::Ok) {
            report("shell: debug_read = ", status);
            return 1;
        }
        if (actual > sizeof(input)) {
            auto _ = ort::debug_line("shell: debug_read returned an invalid byte count");
            return 1;
        }

        for (usize index = 0; index < actual; ++index) {
            auto const byte = input[index];
            if (byte == '\n') {
                auto _ = ort::debug_write("\n");
                if (line_overflow) {
                    auto _ = ort::debug_line("shell: command line is too long");
                } else {
                    line[line_size] = '\0';
                    std::string_view args[SHELL_ARG_MAX];
                    auto const count = split_words(line, args, SHELL_ARG_MAX);
                    if (count > SHELL_ARG_MAX) {
                        auto _ = ort::debug_line("shell: too many command arguments");
                    } else if (dispatch_command(starter, job, args, count)) {
                        return 0;
                    }
                }
                line_size = 0;
                line_overflow = false;
                auto _ = ort::debug_write(PROMPT);
                continue;
            }
            if (byte == 3) {
                line_size = 0;
                line_overflow = false;
                auto _ = ort::debug_write("^C\n");
                auto _ = ort::debug_write(PROMPT);
                continue;
            }
            if (byte == '\b' || byte == 127) {
                if (line_size != 0) {
                    --line_size;
                    line_overflow = false;
                    auto _ = ort::debug_write("\b \b");
                }
                continue;
            }
            if (byte >= 32 && byte < 127) {
                if (line_size + 1 < sizeof(line)) {
                    line[line_size++] = byte;
                    auto _ = ort::debug_write(std::string_view{&byte, 1});
                } else {
                    line_overflow = true;
                }
            }
        }
    }
}
} // namespace

/// Finding the system calls is not this program's problem: the calls below
/// are undefined symbols that the kernel binds to the vDSO's definitions while
/// it loads this image (elfload::Binder).
auto main(int, char **) -> int {
    auto _ = ort::debug_line(BANNER);

    // The namespace first: on dev the root gave this shell /boot and /svc as
    // NsDir handles, which fsio binds here, at the start of main. On
    // dbg_1 there are none, and nothing is bound.
    if (auto const status = fsio::install(); status != ours::Status::Ok) {
        report("shell: installing the namespace FAILED: ", status);
    }
    // What tells the two images apart is what this shell was handed: on dev
    // a namespace with /boot in it, on dbg_1 the BootFS and VMEX (design
    // "Events: dev.dbg_1").
    auto const component = is_bound(BOOT_PATH);
    auto const job = ort::take_startup_handle(ours::HandleType::JobDefault, 0);
    auto const bootfs = ort::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    // VMEX lets the shell make BootFS-backed program images executable. Only
    // where userboot started it (dbg_1): on dev the root alone holds VMEX,
    // and the files it serves under /boot/bin are executable already.
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    // The vDSO's VMO is what the launcher maps into each process it starts.
    auto const vdso_vmo = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    // The Debug resource, for serial input: a startup handle where userboot
    // started this shell, and through /svc where the root did, which hands
    // it to nobody at start (D19).
    auto debug_resource = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                   u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    // The root VMA, which whoever started this put in the processargs.
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (component) {
        auto _ = ort::debug_line("shell: started by the root, with /boot and /svc");
        if (auto const fetched = debug_resource_from_svc(); fetched.is_ok()) {
            debug_resource = fetched.unwrap();
        } else {
            report("shell: Debug.Get FAILED: ", fetched.unwrap_err());
        }
    } else {
        auto _ = ort::debug_line("shell: started by userboot");
    }
    if (vmex != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vmex resource ok");
    } else {
        auto _ = ort::debug_line("shell: started without vmex");
    }

    auto shell_job = ours::Uandle::Invalid;
    if (job != ours::Uandle::Invalid) {
        if (auto const status = ours::handle_duplicate(job, ours::Rights::SameRights, &shell_job);
            status != ours::Status::Ok) {
            report("shell: duplicating job for the command shell = ", status);
        }
    }

    describe_bootfs(bootfs);
    auto starter = Starter{.library = {}, .component = component, .bootfs_file = {}};
    // Not a component: started by userboot, with the BootFS and VMEX to
    // start programs with itself. make_launcher() says FAILED if they are
    // missing, since such a shell could start nothing.
    if (job != ours::Uandle::Invalid && !component) {
        starter.library = make_launcher(bootfs, vmex, vdso_vmo, vma);
    }
    auto &launcher = starter.library;
    shell::LaunchProgram test_launcher = nullptr;
    Launcher const *test_library = nullptr;
    if (launcher.executable != ours::Uandle::Invalid) {
        test_launcher = launch_for_tests;
        test_library = &launcher;
    }
    shell::tests::Inputs const test_inputs{
        .job = job,
        .bootfs = bootfs,
        .vmex = vmex,
        .vdso = vdso_vmo,
        .vma = vma,
        .launch_program = test_launcher,
        .launcher_context = &launcher,
        .library = test_library,
        .component = component,
    };
    shell::tests::run(test_inputs);

    static Registry registry;
    auto const status = registry.fetch();

    auto _ = ort::debug_line("--- commands ---");
    auto _ = ort::debug_write(registry.text());
    if (status == ours::Status::NoSpace) {
        auto _ = ort::debug_line("(truncated: the registry outgrew this program's buffer)");
    } else if (status != ours::Status::Ok) {
        auto _ = ort::debug_line("(command_list failed)");
    }
    auto _ = ort::debug_line("--- end ---");

    if (debug_resource == ours::Uandle::Invalid) {
        if (launcher.executable != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(launcher.executable);
        }
        if (shell_job != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(shell_job);
        }
        return 0;
    }
    // The BootFS for `ls` on dbg_1: the library's mapping when there is one,
    // and a read-only mapping of the shell's own otherwise. dev lists its
    // namespace instead.
    if (launcher.executable != ours::Uandle::Invalid) {
        starter.bootfs_file = launcher.bootfs_file;
    } else if (!component && bootfs != ours::Uandle::Invalid && vma != ours::Uandle::Invalid) {
        if (auto const mapped = launch::map_bootfs(
                bootfs, vma, {.report = print_launch_report, .context = nullptr});
            mapped.is_ok()) {
            starter.bootfs_file = mapped.unwrap();
        } else {
            report("shell: mapping the BootFS for ls = ", mapped.unwrap_err());
        }
    }
    auto const result = interactive_shell(debug_resource, shell_job, starter);
    if (launcher.executable != ours::Uandle::Invalid) {
        auto _ = ours::handle_close(launcher.executable);
    }
    if (shell_job != ours::Uandle::Invalid) {
        auto _ = ours::handle_close(shell_job);
    }
    return result;
}
