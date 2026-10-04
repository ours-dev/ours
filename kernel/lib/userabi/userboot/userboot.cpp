/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// userboot -- the first program vmours runs in ring 3, and the one that
/// starts everything after it.
///
/// Zircon counterpart: //zircon/kernel/lib/userabi/userboot (main.rs). The
/// kernel always starts this program first (`kernel.select.userboot` picks
/// which build of it, and defaults to this one); what runs *next* is this
/// program's decision, made from `userboot.next=<name>` on the image's
/// command line. Keeping the two apart is the point: userboot is userboot,
/// init is init, and userboot starts init. An image that wants a different
/// system says so with `userboot.next`, not by putting another program in
/// userboot's place.
///
/// It runs on the runtime like every other user program (//system/lib/
/// runtime): _start reads the processargs message the kernel queued on the
/// bootstrap channel (make_bootstrap() in //kernel/lib/userabi/src/
/// userboot.cpp) and calls main(). That message carries the root job, the
/// BootFS, and -- as environment strings -- the image's user-space options
/// (collect_user_options() in //kernel/lib/userabi/src/userboot.cpp), which
/// is how this program sees a command line it has no other way to reach.
/// The `userboot.*` ones are its own; the rest (`ours.autorun.boot`, which
/// init reads) it hands on to the next program as that program's
/// environment. Zircon's userboot reads its options out of the ZBI's
/// CMDLINE item instead (get_options() in main.rs) and hands the whole ZBI
/// on; the kernel forwarding tokens spares both programs a boot-container
/// parser.
///
/// It starts the next program itself, with the user-space launcher
/// (launch::launch_from_bootfs(), U8): the BootFS mapped into its own address
/// space and made executable with vmex. The kernel loads this program and
/// no other; Zircon's userboot likewise loads its next program from the
/// BootFS itself (launch_program() in main.rs). The mapping and the
/// executable copy go with this
/// process when it exits.

#include <limits>
#include <string_view>

#include <elfload/diagnostics.hpp>
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <launch/plan.hpp>
#include <obi/container_view.hpp>
#include <obi/package_format.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/status.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace {
/// The option naming the program to start next. Only the `key=value` form
/// means anything, since a bare `userboot.next` names no program;
/// ort::ProcessArgs::option() reads both as empty. Zircon's key of the
/// same name (parse_option() in main.rs).
constexpr std::string_view NEXT_OPTION{"userboot.next"};

/// The family of options that are this program's own and stop here; every
/// other environment string is the next program's and is handed on.
constexpr std::string_view USERBOOT_OPTION_PREFIX{"userboot."};

/// The environment strings the next program gets: this program's own less
/// its `userboot.*` options.
///
/// Views into this process's processargs, which the runtime keeps for the
/// life of the process; held only until start_next() has copied them into
/// the next program's message.
///
/// Room for every option the kernel may relay (ours::MAX_USER_OPTIONS), so a
/// subset of them always fits.
struct HandedOn {
    std::string_view entries[ours::MAX_USER_OPTIONS];
    u32 count{0};
};

/// Prints "<what><status name>".
auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
}

auto to_str(std::string_view const text) -> std::string_view {
    return {text.data(), text.size()};
}

[[noreturn]]
auto halt() -> void {
    // `pause` is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects undefined behaviour, and clang acts on
    // that licence: at -O2 it deletes `for (;;) {}` outright and lets control
    // fall off the end of the function. An `asm volatile` is a side effect the
    // optimiser must keep.
    //
    // Only the proof-of-life image comes here; every path that was asked to
    // start something ends in process_exit instead (see main). With nothing
    // to hand on, staying keeps the one thing left to observe -- the timer
    // interrupts the kernel goes on taking from user space -- as evidence
    // that ring 3 resumed after its last system call.
    for (;;) {
        __asm__ volatile("pause");
    }
}

/// The environment strings that are not this program's own options.
///
/// Not a filter the kernel could apply for it: the kernel sends one message,
/// to this program, and has no say in what the next one gets. Zircon's
/// userboot passes on everything instead -- the whole ZBI -- but a program
/// there asks component_manager for the options it wants; here the
/// environment *is* the options, so what `userboot.next` was is none of
/// init's business. One that does not fit -- only possible if something
/// other than the kernel wrote this program's message -- is dropped and
/// named, as the kernel does with a token it has no room for.
auto collect_handed_on() -> HandedOn {
    HandedOn handed_on;
    for (auto const entry: ort::process_args().environ()) {
        if (entry.starts_with(USERBOOT_OPTION_PREFIX)) {
            continue;
        }
        if (handed_on.count == ours::MAX_USER_OPTIONS) {
            auto _ =
                ort::debug_line({"userboot: dropping ", to_str(entry), ": no room to hand it on"});
            continue;
        }
        handed_on.entries[handed_on.count++] = entry;
    }
    return handed_on;
}

/// The rights the BootFS is handed on with: the ones the kernel gave it when
/// it built the BootFS itself. No Write: every program's code is mapped from
/// slices of this VMO, through an executable duplicate of it, so a holder
/// that could write it could change the code of every program started from
/// it.
constexpr auto BOOTFS_RIGHTS =
    ours::Rights::Basic | ours::Rights::Read | ours::Rights::Map | ours::Rights::GetProperty;

/// |length| rounded up to whole pages, as a mapping is; OutOfRange if it
/// wraps or does not fit a usize.
auto mapping_size(u64 const length) -> ustl::Result<usize, ours::Status> {
    auto const rounded = ustl::mm::align_up(length, launch::PAGE_SIZE);
    if (rounded < length || rounded > std::numeric_limits<usize>::max()) {
        return ustl::err(ours::Status::OutOfRange);
    }
    return ustl::ok(static_cast<usize>(rounded));
}

/// The user BootFS, unpacked from |image|, the boot image the kernel handed
/// over (VmoBootdata), into a VMO of its own: a read-only handle to it, or
/// the step that failed, said on the console.
///
/// The image is mapped read-only and parsed as an OBI container; its
/// StorageBootFs item is the BootFS, LZ4-compressed. A VMO of the item's
/// decompressed size is mapped read-write and the item materialized into
/// it (obi::ItemView::materialize(), which decompresses or copies). Then
/// Write goes: the handle handed back is a duplicate without it, and the
/// writable one is closed.
///
/// The read-write mapping is taken down as soon as the BootFS is in it, as
/// Zircon's userboot does (get_bootfs_vmo() in
/// src/bringup/lib/userboot/rust/src/bootfs.rs, which this follows). Dropping
/// Write from the handle is not enough on its own: a writable mapping still
/// writes the VMO, and the launcher in every later program reads the BootFS
/// in place, trusting that nobody can write it (the component-framework
/// design's I6). The image's read-only mapping stays until this process
/// exits, which it does once the next program is started; Zircon's userboot
/// keeps its ZBI mapped too.
auto unpack_bootfs(ours::Uandle const image, ours::Uandle const vma)
    -> ustl::Result<ours::Uandle, ours::Status> {
    using ours::Status;

    // The header through the VMO first: it alone says how long the image is.
    obi::ContainerHeader header{};
    if (auto const status = ours::vmo_read(image, &header, 0, sizeof(header));
        status != Status::Ok) {
        report("userboot: reading the boot image header = ", status);
        return ustl::err(status);
    }
    auto const image_size = mapping_size(header.container_size);
    if (image_size.is_err()) {
        report("userboot: sizing the boot image = ", image_size.unwrap_err());
        return ustl::err(image_size.unwrap_err());
    }
    ours::VirtAddr image_address = 0;
    if (auto const status = ours::vma_map(vma, ours::VmOptions::PermRead, 0, image, 0,
                                          image_size.unwrap(), &image_address);
        status != Status::Ok) {
        report("userboot: mapping the boot image = ", status);
        return ustl::err(status);
    }
    // ContainerView checks what the header claims against the bytes there.
    auto const container = obi::ContainerView::from(reinterpret_cast<u8 const *>(image_address),
                                                    static_cast<usize>(header.container_size));
    if (container.is_err()) {
        report("userboot: checking the boot image = ", container.unwrap_err());
        return ustl::err(container.unwrap_err());
    }

    // The first StorageBootFs item, as Zircon's userboot takes the first
    // STORAGE_BOOTFS.
    auto found = ustl::Option<obi::ItemView>{ustl::NONE};
    for (usize index = 0; index < container.unwrap().item_count() && found.is_none(); ++index) {
        auto const candidate = container.unwrap().item(index);
        if (candidate.is_err()) {
            report("userboot: reading a boot image item = ", candidate.unwrap_err());
            return ustl::err(candidate.unwrap_err());
        }
        if (candidate.unwrap().type() == obi::ItemType::StorageBootFs) {
            found = ustl::Option<obi::ItemView>(candidate.unwrap());
        }
    }
    if (found.is_none() || found.value().format() != obi::ItemFormat::BootFs) {
        auto _ = ort::debug_line("userboot: the boot image has no BootFS item");
        return ustl::err(Status::NotFound);
    }
    auto const &item = found.value();

    auto const bootfs_size = mapping_size(item.memory_size());
    if (bootfs_size.is_err()) {
        report("userboot: sizing the BootFS = ", bootfs_size.unwrap_err());
        return ustl::err(bootfs_size.unwrap_err());
    }
    auto writable = ours::Uandle::Invalid;
    if (auto const status =
            ours::vmo_create(bootfs_size.unwrap(), ours::VmoOptions::None, &writable);
        status != Status::Ok) {
        report("userboot: creating the BootFS VMO = ", status);
        return ustl::err(status);
    }
    ours::VirtAddr bootfs_address = 0;
    auto status = ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0,
                                writable, 0, bootfs_size.unwrap(), &bootfs_address);
    if (status != Status::Ok) {
        report("userboot: mapping the BootFS VMO = ", status);
    } else {
        if (auto const materialized =
                item.materialize({reinterpret_cast<u8 *>(bootfs_address), bootfs_size.unwrap()});
            materialized.is_err()) {
            status = materialized.unwrap_err();
            report("userboot: decompressing the BootFS = ", status);
        }
        // Done with whether or not it worked: the BootFS is read through its
        // VMO from here on, and never written again.
        if (auto const unmapped = ours::vma_unmap(vma, bootfs_address, bootfs_size.unwrap());
            unmapped != Status::Ok && status == Status::Ok) {
            status = unmapped;
            report("userboot: unmapping the writable BootFS = ", status);
        }
    }
    auto readonly = ours::Uandle::Invalid;
    if (status == Status::Ok) {
        status = ours::handle_duplicate(writable, BOOTFS_RIGHTS, &readonly);
        if (status != Status::Ok) {
            report("userboot: dropping Write from the BootFS = ", status);
        }
    }
    auto _ = ours::handle_close(writable);
    if (status != Status::Ok) {
        return ustl::err(status);
    }

    char size_text[18];
    auto _ = ort::debug_line({"userboot: unpacked the BootFS from the boot image, ",
                              ort::hex(item.memory_size(), size_text), " bytes"});
    return ustl::ok(readonly);
}

/// The launcher's failures, on the debug console: only the errors, each
/// naming the step.
auto print_launch_error(void *, elfload::Report const &report) -> void {
    if (report.severity != elfload::Severity::Error) {
        return;
    }
    std::string_view separator;
    std::string_view name;
    if (!report.name.empty()) {
        separator = ": ";
        name = {report.name.data(), report.name.size()};
    }
    auto _ = ort::debug_line(
        {"userboot: launch failed: ", {report.what.data(), report.what.size()}, separator, name});
}

/// Starts |name| from the BootFS with |launcher| under |job|, handing it
/// |job|, |bootfs|, |vmex|, |vdso| and |debug| as its startup handles --
/// exactly what the kernel handed this program, so the next program starts
/// as it would have had the kernel started it directly. argv is {|name|};
/// the environment is |environ|, the options that were not this program's.
///
/// The process is started before its processargs are written, the reverse
/// of the kernel's order (make_bootstrap()): |job| has to go into the
/// message, and channel_write takes every handle it sends out of this
/// process, so written first it would leave nothing to launch under. The
/// other way round costs nothing -- the runtime waits for READABLE before
/// it reads -- and saves a handle_duplicate that would only leave userboot
/// holding a second root job handle it has no use for.
///
/// On success the handles have left this process. On failure to launch they
/// are still here, and the caller decides what to do with them; a failure
/// after the launch loses them with the message, and the new process,
/// finding PEER_CLOSED and no message, exits with ort::EXIT_START_FAILED.
auto start_next(launch::BootfsLauncher const &launcher, ours::Uandle const job,
                ours::Uandle const bootfs, ours::Uandle const vmex, ours::Uandle const vdso,
                ours::Uandle const debug, std::string_view const name, HandedOn const &environ)
    -> ours::Status {
    using ours::Status;

    // The child's root VMA first, filled in once the launcher has made the
    // child and handed it back; its handle_info is known already, so the
    // message can be encoded before.
    ours::Uandle handles[6] = {ours::Uandle::Invalid, job};
    u32 infos[6] = {ours::handle_info(ours::HandleType::VmaRoot, 0),
                    ours::handle_info(ours::HandleType::JobDefault, 0)};
    u32 count = 2;
    if (bootfs != ours::Uandle::Invalid) {
        handles[count] = bootfs;
        infos[count++] = ours::handle_info(ours::HandleType::VmoBootfs, 0);
    }
    // The vDSO's VMO: the next program starts processes too, and maps the
    // vDSO into them (K23).
    if (vdso != ours::Uandle::Invalid) {
        handles[count] = vdso;
        infos[count++] = ours::handle_info(ours::HandleType::VmoVdso, 0);
    }
    // Handed on after this program's own use of it, the BootFS's executable
    // copy: the next program makes its own (design D3).
    if (vmex != ours::Uandle::Invalid) {
        handles[count] = vmex;
        infos[count++] =
            ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_VMEX_BASE));
    }
    if (debug != ours::Uandle::Invalid) {
        handles[count] = debug;
        infos[count++] =
            ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    }

    // The header, six handle_infos, a name of at most 63 bytes and its NUL
    // -- the kernel keeps program names in 64 -- and the environment, a
    // subset of what the kernel relayed and so within USER_OPTIONS_BYTES:
    // the same sum the kernel sizes this program's own message by
    // (make_bootstrap()), so encoding cannot fail for want of room.
    constexpr usize MESSAGE_BYTES =
        sizeof(ours::ProcArgs) + 6 * sizeof(u32) + 64 + ours::USER_OPTIONS_BYTES;
    u8 message[MESSAGE_BYTES];
    u32 length = 0;
    std::string_view const args[] = {name};
    if (auto const status = ours::encode_process_args(
            message, {infos, count}, args, {environ.entries, environ.count}, {}, &length);
        status != Status::Ok) {
        return status;
    }

    ours::Uandle here = ours::Uandle::Invalid;
    ours::Uandle there = ours::Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        return status;
    }
    // |there| leaves this process whatever the launch answers.
    auto const started = launch::launch_from_bootfs(
        launcher, job, name, there, {.report = print_launch_error, .context = nullptr});
    if (started.is_err()) {
        auto _ = ours::handle_close(here);
        return started.unwrap_err();
    }
    handles[0] = started.unwrap().root_vma;
    // userboot does not wait for the next program: it exits once the message
    // is written. Closing the handle does not stop a started process.
    auto _ = ours::handle_close(started.unwrap().process);
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, message, length, handles, count);
    auto _ = ours::handle_close(here);
    return written;
}
} // namespace

/// Says so when the kernel handed over a crash record from the previous boot
/// (HandleType::VmoCrashlog): a read-only VMO holding the record's text, the
/// panic's message on its first line. userboot only reports that line and
/// lets the VMO go; nothing after it reads the record yet. Zircon's userboot
/// passes its "crashlog" VMO on to the next program instead.
auto report_crashlog() -> void {
    auto const crashlog = ort::take_startup_handle(ours::HandleType::VmoCrashlog, 0);
    if (crashlog == ours::Uandle::Invalid) {
        return;
    }
    // The VMO is whole pages, so this much can always be read; past the
    // record's end it is zeros.
    char first[160] = {};
    if (ours::vmo_read(crashlog, first, 0, sizeof(first)) == ours::Status::Ok) {
        auto line = std::string_view(first, sizeof(first));
        line = line.substr(0, line.find_first_of(std::string_view("\n\0", 2)));
        auto _ = ort::debug_line({"userboot: the previous boot's crash record: ", line});
    }
    auto _ = ours::handle_close(crashlog);
}

auto main(int, char **) -> int {
    auto const next = ort::process_args().option(NEXT_OPTION);
    if (next.empty()) {
        // No program named: the proof-of-life image. Reaching this line at
        // all is what it proves -- the kernel built a process, mapped this
        // ELF and the vDSO into it, and the runtime read its processargs.
        auto _ = ort::debug_line("userboot: hello from ring 3");
        halt();
    }

    // From here on every path returns, and the runtime turns main's value
    // into process_exit: userboot's work is done once the next program has
    // what it needs, and a userboot that cannot give it that has nothing
    // else to do either. Zircon's userboot ends the same way on success and
    // on failure (main() in main.rs logs the error and returns). Its exit
    // takes nothing with it: the next program lives under the root job, not
    // under userboot, and holds the only handles userboot had.
    auto const job = ort::take_startup_handle(ours::HandleType::JobDefault, 0);
    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("userboot: started without the root job");
        return 1;
    }
    auto const image = ort::take_startup_handle(ours::HandleType::VmoBootdata, 0);
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    auto const debug = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    auto const vdso = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    report_crashlog();

    // Without the boot image there is no BootFS, and without the BootFS
    // nothing to start the next program from: the kernel loads no program
    // but this one.
    if (image == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid ||
        vdso == ours::Uandle::Invalid || vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line({"userboot: cannot start ", to_str(next),
                                  " (missing the boot image, vmex, vDSO or VmaRoot)"});
        return 1;
    }
    auto const unpacked = unpack_bootfs(image, vma);
    // Read once; the BootFS is the next program's, the rest of the image
    // nobody's.
    auto _ = ours::handle_close(image);
    if (unpacked.is_err()) {
        return 1;
    }
    auto const bootfs = unpacked.unwrap();
    auto const made = launch::make_bootfs_launcher(
        bootfs, vmex, vdso, vma, {.report = print_launch_error, .context = nullptr});
    if (made.is_err()) {
        report("userboot: setting the launcher up = ", made.unwrap_err());
        return 1;
    }
    auto const &launcher = made.unwrap();

    auto _ = ort::debug_line({"userboot: starting ", to_str(next)});
    auto const handed_on = collect_handed_on();
    for (auto index = u32(0); index < handed_on.count; ++index) {
        auto _ = ort::debug_line(
            {"userboot: passing ", to_str(handed_on.entries[index]), " to ", to_str(next)});
    }
    auto const status = start_next(launcher, job, bootfs, vmex, vdso, debug, next, handed_on);
    // The executable copy was only for the launch; the next program makes
    // its own from the BootFS handed on.
    auto _ = ours::handle_close(launcher.executable);
    if (status != ours::Status::Ok) {
        report("userboot: starting the next program = ", status);
        return 1;
    }
    auto _ = ort::debug_line({"userboot: started ", to_str(next)});
    return 0;
}
