/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// routing_checks -- the root's on-target test component
/// (component-framework design D24), the counterpart of
/// src/sys/component_manager/tests/rights and tests/routing in Fuchsia.
///
/// dev's meta/root.cm starts it as an eager child. Its manifest
/// (routing_checks.cm, beside this file) uses the root's built-in `boot` four
/// ways, and root.cm offers each:
///
///   /boot     rx*, the whole of /boot;
///   /boot-r   r*, the same directory offered under another name with `as`;
///   /bin      rx*, the `bin` subdirectory of /boot, joined by the use's subdir;
///   /denied   rx*, through an offer of only r*: the route narrows the rights
///             the wrong way, so the root answers AccessDenied and closes
///             the entry with an epitaph before this program runs.
///
/// It checks what each entry gives through fsio: File.Read,
/// Directory.ReadDirents, File.GetBackingMemory with and without EXECUTE,
/// the EXECUTE gate on a file that is not a program, and the AccessDenied
/// entry, whose status every open() under it answers. Each failure prints a
/// line with FAILED in it, which fails ours-verify's boot check. All passing,
/// it prints `routing_checks: passed` and exits 0, which the root logs as
/// `init: routing_checks exited with 0`.
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <fsio/fsio.hpp>
#include <fuchsia_io/fuchsia_io.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>

namespace {
using fuchsia_io::Flags;
using fuchsia_io::VmoFlags;
using ours::Status;
using ours::Uandle;

constexpr auto PAGE = usize{4096};

/// This program, under each entry it reaches it by.
constexpr auto SELF_IN_BOOT = std::string_view{"/boot/bin/routing_checks"};
constexpr auto SELF_IN_BOOT_R = std::string_view{"/boot-r/bin/routing_checks"};
constexpr auto SELF_IN_BIN = std::string_view{"/bin/routing_checks"};
/// A file of /boot that is not under bin/, so not executable.
constexpr auto MANIFEST = std::string_view{"/boot/meta/routing_checks.cm"};

/// How many checks failed.
constinit u32 failures = 0;

auto status_name(Status const status) -> std::string_view {
    return ours::to_string(status);
}

/// Says that |what| answered |got| where |expected| was due, once, with
/// FAILED in the line.
auto fail(std::string_view const what, Status const got, Status const expected) -> void {
    failures += 1;
    auto _ = ort::debug_line({"routing_checks: ", what, " = ", status_name(got), " (expected ",
                              status_name(expected), ") FAILED"});
}

/// Checks that |what| answered |expected|; true when it did.
auto expect(std::string_view const what, Status const got, Status const expected) -> bool {
    if (got != expected) {
        fail(what, got, expected);
        return false;
    }
    return true;
}

/// A check that is not about a status: |what| was not so.
auto fail(std::string_view const what) -> void {
    failures += 1;
    auto _ = ort::debug_line({"routing_checks: ", what, " FAILED"});
}

/// The status of |result|: Ok, or its error.
template <typename T>
auto status_of(ustl::Result<T, Status> const &result) -> Status {
    if (result.is_ok()) {
        return Status::Ok;
    }
    return result.unwrap_err();
}

/// Opens |path| with |flags|, expecting Ok, and returns the client end, or
/// Invalid after saying why not.
auto open_or_fail(std::string_view const path, Flags const flags) -> Uandle {
    auto const opened = fsio::open(path, flags);
    if (!expect(path, status_of(opened), Status::Ok)) {
        return Uandle::Invalid;
    }
    return opened.unwrap();
}

/// File.Read: the first four bytes of this program, through |path|, are an
/// ELF header's.
auto check_read(std::string_view const path) -> void {
    auto const file = open_or_fail(path, Flags::PermReadBytes);
    if (file == Uandle::Invalid) {
        return;
    }
    std::array<u8, 4> magic{};
    auto const read = fsio::read(file, magic);
    if (expect("read", status_of(read), Status::Ok)) {
        if (read.unwrap() != magic.size() || magic[0] != 0x7f || magic[1] != 'E' ||
            magic[2] != 'L' || magic[3] != 'F') {
            fail("read: the first four bytes are not \"\\x7fELF\"");
        }
    }
    expect("close", fsio::close(file), Status::Ok);
}

/// What one ReadDirents listing held.
struct Listing {
    bool dot_first = false;
    bool found = false;
    fuchsia_io::DirentType type = fuchsia_io::DirentType::Unknown;
};

/// Directory.ReadDirents on |path|: "." comes first, as the Rust vfs lists
/// a directory, and |name| is there, with |type|.
auto check_read_dirents(std::string_view const path, std::string_view const name,
                        fuchsia_io::DirentType const type) -> void {
    auto const directory =
        open_or_fail(path, Flags::ProtocolDirectory | Flags::PermEnumerate | Flags::PermReadBytes);
    if (directory == Uandle::Invalid) {
        return;
    }
    auto listing = Listing{};
    auto first = true;
    std::array<u8, fuchsia_io::MAX_BUF> buffer{};
    for (;;) {
        auto const read = fsio::read_dirents(directory, buffer);
        if (!expect("read_dirents", status_of(read), Status::Ok) || read.unwrap() == 0) {
            break;
        }
        auto reader = fuchsia_io::DirentReader(std::span{buffer}.first(read.unwrap()));
        for (auto next = reader.next(); next.is_ok() && next->is_some(); next = reader.next()) {
            auto const &dirent = next.unwrap().value();
            if (first) {
                listing.dot_first = dirent.name == ".";
                first = false;
            }
            if (dirent.name == name) {
                listing.found = true;
                listing.type = dirent.type;
            }
        }
    }
    if (!listing.dot_first) {
        fail("read_dirents: \".\" is not the first entry");
    }
    if (!listing.found || listing.type != type) {
        auto _ = ort::debug_line({"routing_checks: in ", path, ": ", name});
        fail("read_dirents: the entry above is missing or of the wrong type");
    }
    expect("close", fsio::close(directory), Status::Ok);
}

/// Maps one page of |vmo| into |vma| with |options|, unmaps it again, and
/// says what the map answered: whether the VMO holds the rights the options
/// need, Execute among them.
auto try_map(Uandle const vma, Uandle const vmo, ours::VmOptions const options) -> Status {
    auto address = ours::VirtAddr{0};
    auto const mapped = ours::vma_map(vma, options, 0, vmo, 0, PAGE, &address);
    if (mapped == Status::Ok) {
        auto _ = ours::vma_unmap(vma, address, PAGE);
    }
    return mapped;
}

/// File.GetBackingMemory without and with EXECUTE (design "Events: a
/// component opening a file in /boot", step 4): the VMO carries Execute
/// exactly when it was asked for on a connection that holds EXECUTE, and
/// then this process can map it executable.
auto check_backing_memory(Uandle const vma) -> void {
    constexpr auto R = ours::VmOptions::PermRead;
    constexpr auto RX = ours::VmOptions::PermRead | ours::VmOptions::PermExecute;

    // Without EXECUTE: a readable VMO, which refuses an executable mapping.
    if (auto const file = open_or_fail(SELF_IN_BOOT, Flags::PermReadBytes);
        file != Uandle::Invalid) {
        auto const vmo = fsio::get_backing_memory(file, VmoFlags::Read);
        if (expect("get_backing_memory(READ)", status_of(vmo), Status::Ok)) {
            expect("map(R) of a READ VMO", try_map(vma, vmo.unwrap(), R), Status::Ok);
            expect("map(RX) of a READ VMO", try_map(vma, vmo.unwrap(), RX), Status::AccessDenied);
            auto _ = ours::handle_close(vmo.unwrap());
        }
        // The connection holds no EXECUTE, so it cannot hand one out.
        expect("get_backing_memory(READ|EXECUTE) without EXECUTE",
               status_of(fsio::get_backing_memory(file, VmoFlags::Read | VmoFlags::Execute)),
               Status::AccessDenied);
        expect("close", fsio::close(file), Status::Ok);
    }

    // With it, as fdio_get_vmo_exec asks (READ|EXECUTE|PRIVATE_CLONE).
    if (auto const file = open_or_fail(SELF_IN_BOOT, Flags::PermReadBytes | Flags::PermExecute);
        file != Uandle::Invalid) {
        auto const vmo = fsio::get_backing_memory(file, VmoFlags::Read | VmoFlags::Execute |
                                                            VmoFlags::PrivateClone);
        if (expect("get_backing_memory(READ|EXECUTE|PRIVATE_CLONE)", status_of(vmo), Status::Ok)) {
            expect("map(RX) of an EXECUTE VMO", try_map(vma, vmo.unwrap(), RX), Status::Ok);
            auto _ = ours::handle_close(vmo.unwrap());
        }
        expect("close", fsio::close(file), Status::Ok);
    }
}

/// What a refused Open answers: open() itself is Ok, since Open is one-way,
/// and the first call on the new connection reads the epitaph.
auto refused(std::string_view const path, Flags const flags) -> Status {
    auto const opened = fsio::open(path, flags);
    if (opened.is_err()) {
        return opened.unwrap_err();
    }
    std::array<u8, 1> byte{};
    auto const status = status_of(fsio::read(opened.unwrap(), byte));
    auto _ = ours::handle_close(opened.unwrap());
    return status;
}

/// Rights a connection does not hold are refused, wherever they come from.
auto check_execute_refused() -> void {
    // r* has no EXECUTE, so nothing opened through it can ask for it.
    expect("open(EXECUTE) under /boot-r", refused(SELF_IN_BOOT_R, Flags::PermExecute),
           Status::AccessDenied);
    // /boot holds EXECUTE, but a file outside bin/ is not executable: the
    // EXECUTE gate (the Rust vfs's file/common.rs).
    expect("open(EXECUTE) of a manifest", refused(MANIFEST, Flags::PermExecute),
           Status::AccessDenied);
}

/// The entry whose route the root refused: its status, read once from the
/// epitaph, then answered by every later open() from the binding.
auto check_denied() -> void {
    for (auto const path: {"/denied/bin/routing_checks", "/denied", "/denied/bin"}) {
        expect(path, status_of(fsio::open(path, Flags::PermReadBytes)), Status::AccessDenied);
    }
}
} // namespace

auto main(int, char **) -> int {
    if (!expect("fsio::install()", fsio::install(), Status::Ok)) {
        auto _ = ort::debug_line("routing_checks: FAILED");
        return 1;
    }
    if (auto const bound = fsio::installed_namespace().size(); bound != 4) {
        fail("the namespace does not hold the manifest's four directory uses");
    }
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);

    check_read(SELF_IN_BOOT);
    check_read(SELF_IN_BOOT_R);
    check_read(SELF_IN_BIN);
    check_read_dirents("/boot", "bin", fuchsia_io::DirentType::Directory);
    check_read_dirents("/boot", "meta", fuchsia_io::DirentType::Directory);
    check_read_dirents("/bin", "routing_checks", fuchsia_io::DirentType::File);
    if (vma == Uandle::Invalid) {
        fail("started without VmaRoot");
    } else {
        check_backing_memory(vma);
    }
    check_execute_refused();
    check_denied();

    if (failures != 0) {
        char count[10];
        auto _ = ort::debug_line(
            {"routing_checks: ", ort::decimal(failures, count), " check(s) FAILED"});
        return 1;
    }
    auto _ = ort::debug_line("routing_checks: passed");
    return 0;
}
