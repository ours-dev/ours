/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// A user-mode command shell that talks to the kernel only through the vDSO.
///
/// It provides a prompt, bounded line editing, built-ins, and foreground
/// launching of BootFS programs. The syscall checks below still run before
/// the prompt, so boot verification exercises the same kernel boundary.
///
/// Its system calls are the vDSO's entries, ours_* from <ours/vdso.hpp>,
/// called directly, the checks of the raw ABI (bad pointers, option words)
/// among them. It knows nothing else about the boundary -- not the symbols
/// the loader binds, not that a trap is involved at all.
///
/// It runs on the runtime (//system/lib/ort): the entry point is the
/// runtime's, which reads the processargs message this program was started
/// with and calls main. What main finds among the startup handles says who
/// started it and what to do -- a channel to init (User0) to answer, the root
/// job and BootFS for program launch, VMEX, the vDSO, and the System Debug
/// resource for serial input. The C library it links is mem* and strlen
/// (//system/lib/libc), and check_libc() is where this program makes sure it
/// is there and right.

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
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <launch/program.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>

namespace {
using shell::Launcher;
using shell::spells;

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

/// Answers init, if init started this program.
///
/// Started by init, the User0 startup handle is a channel with "ping" on it
/// (or about to be): read it, print it, answer "pong" -- the other half of
/// the first slice, whose log lines are the completion criterion (design 5).
/// Started by userboot (dbg_1's `userboot.next=bin/shell`) there is no such
/// handle, and nobody to answer.
auto answer_init(ours::Uandle const peer) -> void {
    using ours::Signals;
    using ours::Status;

    if (peer == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: started by userboot; no one to answer");
        return;
    }

    auto observed = Signals::None;
    auto status = ours::object_wait_one(peer, Signals::Readable | Signals::PeerClosed,
                                        ours::INSTANT_INFINITE, &observed);
    char buffer[32] = {};
    u32 actual = 0;
    if (status == Status::Ok) {
        status = ours::channel_read(peer, ours::ChannelReadOptions::None, buffer, nullptr,
                                    sizeof(buffer), 0, &actual, nullptr);
    }
    if (status != Status::Ok) {
        report("shell: reading init's channel = ", status);
        return;
    }
    auto _ = ort::debug_line({"shell: init says \"", std::string_view(buffer, actual), "\""});

    constexpr std::string_view PONG{"pong"};
    status = ours::channel_write(peer, ours::ChannelWriteOptions::None, PONG.data(),
                                 static_cast<u32>(PONG.size()), nullptr, 0);
    if (status != Status::Ok) {
        report("shell: answering = ", status);
    }
}

/// Reads the BootFS header through vmo_read and says how many files it
/// holds, as init does (the design's check for this step, K14). Only the
/// shell userboot starts has a BootFS; init keeps its own.
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

/// The `ls` built-in: every file's name, from the BootFS the launcher mapped
/// (launch::BootfsLauncher::bootfs_file), checked by bootfs::View on the way.
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

/// Room for any command's BootFS name: the directory, a `/`, and a word of
/// a line.
using CommandPath = char[user_programs::DIRECTORY.size() + 1 + SHELL_LINE_BYTES];

/// The BootFS name |command| runs from, built in |path| when it is not
/// |command| itself.
///
/// A command with a `/` in it is a path and is taken as typed. Any other is
/// looked up in the directory programs install to (user_programs::DIRECTORY,
/// "bin"), as a Unix shell finds it on a PATH of one entry: `shell` runs
/// bin/shell. Nothing else is searched. The other top-level files are the
/// vDSO and configuration, which no command names, and a fallback would
/// only turn a mistyped program name into an attempt to run one of them.
auto command_path(std::string_view const command, CommandPath &path) -> std::string_view {
    if (command.find('/') != std::string_view::npos) {
        return command;
    }
    auto const directory = user_programs::DIRECTORY;
    if (directory.size() + 1 + command.size() > sizeof(path)) {
        return {};
    }
    std::memcpy(path, directory.data(), directory.size());
    path[directory.size()] = '/';
    std::memcpy(path + directory.size() + 1, command.data(), command.size());
    return {path, directory.size() + 1 + command.size()};
}

auto launch_command(Launcher const &launcher, ours::Uandle const job,
                    std::span<std::string_view const> const args) -> void {
    using ours::Signals;
    using ours::Status;

    // argv[0] stays as typed, as a Unix shell leaves it; only the lookup
    // takes the full name.
    CommandPath path_buffer;
    auto const path = command_path(args[0], path_buffer);
    if (path.empty()) {
        auto _ = ort::debug_line("shell: command name too long");
        return;
    }

    ours::Uandle bootstrap = ours::Uandle::Invalid;
    ours::Uandle bootstrap_there = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &bootstrap, &bootstrap_there);
        status != Status::Ok) {
        report("shell: launch channel_create = ", status);
        return;
    }

    auto const started = launch_program(launcher, job, path, bootstrap_there);
    if (started.is_err()) {
        auto _ = ours::handle_close(bootstrap);
        return;
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
        return;
    }

    auto root_vma = child.root_vma;
    auto const written = ours::channel_write(bootstrap, ours::ChannelWriteOptions::None, message,
                                             length, &root_vma, 1);
    auto _ = ours::handle_close(bootstrap);
    if (written != Status::Ok) {
        report("shell: writing program processargs = ", written);
        auto _ = ours::handle_close(child.process);
        return;
    }

    auto observed = Signals::None;
    if (auto const status = ours::object_wait_one(child.process, Signals::Terminated,
                                                  ours::INSTANT_INFINITE, &observed);
        status != Status::Ok) {
        report("shell: waiting for program = ", status);
    }
    auto _ = ours::handle_close(child.process);
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

auto dispatch_command(Launcher const &launcher, ours::Uandle const job,
                      std::string_view *const args, usize const count) -> bool {
    if (count == 0) {
        return false;
    }
    if (args[0] == "exit") {
        return true;
    }
    if (args[0] == "help") {
        auto _ = ort::debug_line(
            "shell: builtins: help ls echo commands clear exit; other names run BootFS programs");
        return false;
    }
    if (args[0] == "clear") {
        auto _ = ort::debug_write("\x1b[2J\x1b[H");
        return false;
    }
    if (args[0] == "ls") {
        if (auto const status = list_bootfs(launcher.bootfs_file); status != ours::Status::Ok) {
            report("shell: listing BootFS = ", status);
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
    if (job == ours::Uandle::Invalid || launcher.executable == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: program launching is unavailable");
        return false;
    }
    launch_command(launcher, job, {args, count});
    return false;
}

auto interactive_shell(ours::Uandle const debug_resource, ours::Uandle const job,
                       Launcher const &launcher) -> int {
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
                    } else if (dispatch_command(launcher, job, args, count)) {
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

    // Init starts the shell with a liveness channel and the handles needed to
    // test syscalls and launch BootFS programs.
    auto const peer = ort::take_startup_handle(ours::HandleType::User0, 0);
    auto const job = ort::take_startup_handle(ours::HandleType::JobDefault, 0);
    auto const bootfs = ort::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    // VMEX lets the shell make BootFS-backed program images executable.
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    // The vDSO's VMO is what the launcher maps into each process it starts.
    auto const vdso_vmo = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const debug_resource = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                         u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    // The root VMA, which whoever started this put in the processargs.
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
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

    answer_init(peer);
    describe_bootfs(bootfs);
    auto launcher = Launcher{};
    if (job != ours::Uandle::Invalid && peer == ours::Uandle::Invalid) {
        launcher = make_launcher(bootfs, vmex, vdso_vmo, vma);
    }
    auto startup_to_close = job;
    if (peer != ours::Uandle::Invalid) {
        startup_to_close = peer;
    }
    shell::LaunchProgram test_launcher = nullptr;
    if (launcher.executable != ours::Uandle::Invalid) {
        test_launcher = launch_for_tests;
    }
    shell::tests::Inputs const test_inputs{
        .peer = peer,
        .job = job,
        .bootfs = bootfs,
        .vmex = vmex,
        .vdso = vdso_vmo,
        .vma = vma,
        .startup_to_close = startup_to_close,
        .launch_program = test_launcher,
        .launcher_context = &launcher,
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
    if (launcher.executable == ours::Uandle::Invalid) {
        launcher = make_launcher(bootfs, vmex, vdso_vmo, vma);
    }
    auto const result = interactive_shell(debug_resource, shell_job, launcher);
    if (launcher.executable != ours::Uandle::Invalid) {
        auto _ = ours::handle_close(launcher.executable);
    }
    if (shell_job != ours::Uandle::Invalid) {
        auto _ = ours::handle_close(shell_job);
    }
    return result;
}
