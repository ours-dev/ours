/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// routing_checks -- the root's on-target test component
/// (component-framework design D24): routing and rights, checked from inside
/// a component the root started.
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
/// entry, whose status every open() under it answers.
///
/// Its manifest also uses three protocols, which live in its /svc: the
/// root's built-ins ours.process.Launcher and
/// ours.kernel.DebugResource, which root.cm offers, and
/// ours.test.Unoffered, which nobody offers. It checks that DebugResource.Get
/// hands out the Debug resource, that the unoffered protocol answers
/// NotFound, that the Launcher refuses what it cannot start -- a VMO without
/// Execute, a reserved handle type, too many handles, a malformed message --
/// and that fsio::spawn() refuses a program /boot does not have. Then it
/// spawns itself with the argument `child` through fsio::spawn(), and
/// expects that copy, which only reads one file through the namespace it was
/// handed, to exit 0.
///
/// Its fourth protocol use, ours.test.Echo, is the one route here that ends
/// in another component: root.cm offers it from the sibling echo
/// (//examples/echo), renamed from ours.examples.echo.Echo, which echo
/// exposes from itself. Last, it checks that EchoString comes back from echo
/// through that route, asks echo to stop, and expects the next connect to
/// answer NotFound, since a route to a component that has exited answers so
/// (design D23).
///
/// Each failure prints a line with FAILED in it, which fails ours-verify's
/// boot check. All passing, it prints `routing_checks: passed` and exits 0,
/// which the root logs as `init: routing_checks exited with 0`.
#include <ours/object_info.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <fsio/fsio.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_examples_echo/ours_examples_echo.hpp>
#include <ours_io/ours_io.hpp>
#include <ours_kernel/ours_kernel.hpp>
#include <ours_process/ours_process.hpp>
#include <ustl/option.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using ours_io::Flags;
using ours_io::VmoFlags;

constexpr auto PAGE = usize{4096};

/// This program, under each entry it reaches it by.
constexpr auto SELF_IN_BOOT = std::string_view{"/boot/bin/routing_checks"};
constexpr auto SELF_IN_BOOT_R = std::string_view{"/boot-r/bin/routing_checks"};
constexpr auto SELF_IN_BIN = std::string_view{"/bin/routing_checks"};
/// A file of /boot that is not under bin/, so not executable.
constexpr auto MANIFEST = std::string_view{"/boot/meta/routing_checks.cm"};
/// A program /boot does not have.
constexpr auto MISSING = std::string_view{"/boot/bin/no_such_program"};

/// The protocols its manifest uses, each at /svc/<name> (design D3).
constexpr auto LAUNCHER = std::string_view{"/svc/ours.process.Launcher"};
constexpr auto DEBUG_RESOURCE = std::string_view{"/svc/ours.kernel.DebugResource"};
constexpr auto UNOFFERED = std::string_view{"/svc/ours.test.Unoffered"};
/// echo's protocol, under the name root.cm's offer gives it.
constexpr auto ECHO = std::string_view{"/svc/ours.test.Echo"};

/// What check_echo() sends echo, and expects back.
constexpr auto ECHO_TEXT = std::string_view{"routing_checks"};

/// The argument that makes it the spawned copy (run_child()).
constexpr auto CHILD = std::string_view{"child"};

/// One handle more than the Add* calls may give: a processargs message
/// carries CHANNEL_MAX_MSG_HANDLES, 64, and the Launcher adds two of its own
/// (launch::ProcessLauncherState::MAX_HANDLES).
constexpr auto TOO_MANY_HANDLES = usize{ours::CHANNEL_MAX_MSG_HANDLES} - 2 + 1;

/// How many checks failed.
constinit u32 failures = 0;

/// The txid of the next two-way request this program writes by hand.
constinit u32 txid_counter = 1;

auto next_txid() -> u32 {
    auto const txid = txid_counter;
    txid_counter += 1;
    return txid;
}

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
    ours_io::DirentType type = ours_io::DirentType::Unknown;
};

/// Directory.ReadDirents on |path|: "." comes first, as vfs::Server lists a
/// directory, and |name| is there, with |type|.
auto check_read_dirents(std::string_view const path, std::string_view const name,
                        ours_io::DirentType const type) -> void {
    auto const directory =
        open_or_fail(path, Flags::ProtocolDirectory | Flags::PermEnumerate | Flags::PermReadBytes);
    if (directory == Uandle::Invalid) {
        return;
    }
    auto listing = Listing{};
    auto first = true;
    std::array<u8, ours_io::MAX_BUF> buffer{};
    for (;;) {
        auto const read = fsio::read_dirents(directory, buffer);
        if (!expect("read_dirents", status_of(read), Status::Ok) || read.unwrap() == 0) {
            break;
        }
        auto reader = ours_io::DirentReader(std::span{buffer}.first(read.unwrap()));
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

    // With it, as fsio's spawn asks (READ|EXECUTE|PRIVATE_CLONE).
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
    // EXECUTE gate of vfs::Server's file Open.
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

/// One message read off a channel, owning the handles it carried until
/// take() claims one: what a server answered, or why it closed.
class Incoming final {
  public:
    Incoming() = default;
    Incoming(Incoming const &) = delete;
    auto operator=(Incoming const &) -> Incoming & = delete;

    ~Incoming() {
        for (auto index = u32{0}; index < m_num_handles; ++index) {
            if (m_handles[index] != Uandle::Invalid) {
                auto _ = ours::handle_close(m_handles[index]);
            }
        }
    }

    /// Waits for |channel| to have a message or to close, and reads the
    /// message: PeerClosed when it closed with nothing on it, which is how a
    /// connection closed without an epitaph reads.
    [[nodiscard]]
    auto receive(Uandle const channel) -> Status {
        auto observed = ours::Signals::None;
        if (auto const status =
                ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
            status != Status::Ok) {
            return status;
        }
        if (!(observed & ours::Signals::Readable)) {
            return Status::PeerClosed;
        }
        return ours::channel_read(channel, ours::ChannelReadOptions::None, m_bytes.data(),
                                  m_handles.data(), static_cast<u32>(m_bytes.size()),
                                  static_cast<u32>(m_handles.size()), &m_num_bytes, &m_num_handles);
    }

    [[nodiscard]]
    auto message() const -> wire::Message {
        return {
            .bytes = std::span{m_bytes}.first(m_num_bytes),
            .handles = std::span{m_handles}.first(m_num_handles),
        };
    }

    /// The status of the epitaph this message is; none for any other.
    [[nodiscard]]
    auto epitaph() const -> ustl::Option<Status> {
        auto const header = wire::decode_header(message().bytes, wire::Direction::ToClient);
        if (header.is_err() || !wire::is_epitaph(header.unwrap())) {
            return ustl::NONE;
        }
        auto const status = wire::decode_epitaph(message());
        if (status.is_err()) {
            return ustl::some(Status::InvArgs);
        }
        return ustl::some(status.unwrap());
    }

    /// Takes |handle|, which the message carried: the caller closes it.
    auto take(Uandle const handle) -> void {
        for (auto index = u32{0}; index < m_num_handles; ++index) {
            if (m_handles[index] == handle) {
                m_handles[index] = Uandle::Invalid;
            }
        }
    }

  private:
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_bytes{};
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};
    u32 m_num_bytes = 0;
    u32 m_num_handles = 0;
};

/// Writes |message| on |channel|; its handles go whatever the answer.
auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

/// A new connection to the protocol at |path| in /svc: the client end, or
/// Invalid after saying why not. Open is one-way, so a route that fails
/// shows only on the connection, as an epitaph.
auto connect_or_fail(std::string_view const path) -> Uandle {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (!expect("channel_create",
                ours::channel_create(ours::ChannelCreateOptions::None, &client, &server),
                Status::Ok)) {
        return Uandle::Invalid;
    }
    if (!expect(path, fsio::connect(path, server), Status::Ok)) {
        auto _ = ours::handle_close(client);
        return Uandle::Invalid;
    }
    return client;
}

/// DebugResource.Get answers with the Debug resource: a handle debug_read
/// takes, which it refuses from anything but the System Debug resource.
/// debug_read of 0 bytes returns at once.
auto check_debug_resource() -> void {
    auto const connection = connect_or_fail(DEBUG_RESOURCE);
    if (connection == Uandle::Invalid) {
        return;
    }
    auto const txid = next_txid();
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    auto const request = ours_kernel::encode_debug_resource_get(bytes, {.txid = txid});
    if (!expect("DebugResource.Get", status_of(request), Status::Ok)) {
        auto _ = ours::handle_close(connection);
        return;
    }
    // A write that finds the connection closed is not the answer: the
    // epitaph the root closed it with is, and it is still there to read.
    auto const written = write(connection, request.unwrap());
    Incoming reply;
    if ((written == Status::Ok || written == Status::PeerClosed) &&
        expect("DebugResource.Get's answer", reply.receive(connection), Status::Ok)) {
        if (auto const epitaph = reply.epitaph(); epitaph.is_some()) {
            fail("DebugResource.Get", *epitaph, Status::Ok);
        } else if (auto const decoded =
                       ours_kernel::decode_debug_resource_get_reply(reply.message(), txid);
                   expect("DebugResource.Get's reply", status_of(decoded), Status::Ok) &&
                   expect("DebugResource.Get", decoded->status, Status::Ok)) {
            reply.take(decoded->resource);
            auto actual = usize{0};
            expect("debug_read(the resource DebugResource gave, 0 bytes)",
                   ours::debug_read(decoded->resource, nullptr, 0, &actual), Status::Ok);
            auto _ = ours::handle_close(decoded->resource);
        }
    } else if (written != Status::Ok && written != Status::PeerClosed) {
        fail("DebugResource.Get", written, Status::Ok);
    }
    auto _ = ours::handle_close(connection);
}

/// A protocol use that nobody offers: the connection arrives, since Open is
/// one-way, and the root closes it with the route's status, NotFound.
auto check_unoffered() -> void {
    auto const connection = connect_or_fail(UNOFFERED);
    if (connection == Uandle::Invalid) {
        return;
    }
    Incoming closed;
    if (expect("the epitaph on an unoffered protocol", closed.receive(connection), Status::Ok)) {
        auto const epitaph = closed.epitaph();
        if (epitaph.is_none()) {
            fail("an unoffered protocol answered something other than an epitaph");
        } else {
            expect("an unoffered protocol", *epitaph, Status::NotFound);
        }
    }
    auto _ = ours::handle_close(connection);
}

/// A VMO of this program without Execute, or Invalid after saying why not.
auto readable_self() -> Uandle {
    auto const file = open_or_fail(SELF_IN_BOOT, Flags::PermReadBytes);
    if (file == Uandle::Invalid) {
        return Uandle::Invalid;
    }
    auto const vmo = fsio::get_backing_memory(file, VmoFlags::Read);
    auto _ = ours::handle_close(file);
    if (!expect("get_backing_memory(READ)", status_of(vmo), Status::Ok)) {
        return Uandle::Invalid;
    }
    return vmo.unwrap();
}

/// Sends Launch on |launcher| for |executable| (consumed) under a duplicate
/// of |job|, and returns what it answered: the reply's status, an
/// epitaph's, or PeerClosed. These checks expect no process; one that comes
/// anyway is closed, and says so with a FAILED line.
auto launch_status(Uandle const launcher, Uandle const executable, Uandle const job) -> Status {
    auto launch_job = Uandle::Invalid;
    if (auto const status = ours::handle_duplicate(job, ours::Rights::SameRights, &launch_job);
        status != Status::Ok) {
        auto _ = ours::handle_close(executable);
        return status;
    }
    auto const txid = next_txid();
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, 2> handles{};
    auto const request = ours_process::encode_launch(
        bytes, handles,
        {.txid = txid, .info = {.executable = executable, .job = launch_job, .name = "refused"}});
    if (request.is_err()) {
        auto _ = ours::handle_close(executable);
        auto _ = ours::handle_close(launch_job);
        return request.unwrap_err();
    }
    if (auto const status = write(launcher, request.unwrap());
        status != Status::Ok && status != Status::PeerClosed) {
        return status;
    }
    Incoming reply;
    if (auto const status = reply.receive(launcher); status != Status::Ok) {
        return status;
    }
    if (auto const epitaph = reply.epitaph(); epitaph.is_some()) {
        return *epitaph;
    }
    auto const decoded = ours_process::decode_launch_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return Status::InvArgs;
    }
    if (decoded->status == Status::Ok) {
        fail("a Launch these checks expect refused started a process");
    }
    return decoded->status;
}

/// AddHandles with |count| duplicates of |job|, the first typed |first_type|
/// and the rest User0, each with its index as the argument.
auto add_handles(Uandle const launcher, Uandle const job, usize const count,
                 ours::HandleType const first_type) -> Status {
    std::array<ours_process::HandleInfo, wire::MAX_MESSAGE_HANDLES> infos{};
    auto const close_made = [&infos](usize const made) {
        for (auto index = usize{0}; index < made; ++index) {
            auto _ = ours::handle_close(infos[index].handle);
        }
    };
    for (auto made = usize{0}; made < count; ++made) {
        auto handle = Uandle::Invalid;
        if (auto const status = ours::handle_duplicate(job, ours::Rights::SameRights, &handle);
            status != Status::Ok) {
            close_made(made);
            return status;
        }
        auto type = ours::HandleType::User0;
        if (made == 0) {
            type = first_type;
        }
        infos[made] = {.handle = handle, .id = ours::handle_info(type, static_cast<u16>(made))};
    }
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
    auto const request =
        ours_process::encode_add_handles(bytes, handles, std::span{infos}.first(count));
    if (request.is_err()) {
        close_made(count);
        return request.unwrap_err();
    }
    return write(launcher, request.unwrap());
}

/// The Launcher refuses what it cannot start, each on a connection of its
/// own, and so does fsio::spawn() for a program that is not there.
auto check_launcher_refusals(Uandle const job) -> void {
    // A VMO without Execute: launch::start() maps it with Execute before it
    // reads a byte of it, and the mapping is refused (design D14, I6).
    if (auto const launcher = connect_or_fail(LAUNCHER); launcher != Uandle::Invalid) {
        if (auto const vmo = readable_self(); vmo != Uandle::Invalid) {
            expect("Launch of a VMO without Execute", launch_status(launcher, vmo, job),
                   Status::AccessDenied);
        }
        auto _ = ours::handle_close(launcher);
    }

    // A missing executable: spawn() finds nothing to open, and launches
    // nothing.
    auto const missing_argv = std::array{MISSING};
    expect("spawn of a program /boot does not have",
           status_of(fsio::spawn(job, MISSING, missing_argv)), Status::NotFound);

    // A reserved handle type: VmaRoot is the new process's own region, which
    // only the Launcher has. The mark it sets answers the next Launch with
    // InvArgs before anything is started, so the VMO without Execute, which
    // would answer AccessDenied, is never looked at.
    if (auto const launcher = connect_or_fail(LAUNCHER); launcher != Uandle::Invalid) {
        expect("AddHandles with a VmaRoot",
               add_handles(launcher, job, 1, ours::HandleType::VmaRoot), Status::Ok);
        if (auto const vmo = readable_self(); vmo != Uandle::Invalid) {
            expect("Launch after a reserved handle type", launch_status(launcher, vmo, job),
                   Status::InvArgs);
        }
        auto _ = ours::handle_close(launcher);
    }

    // Too many handles: 63, where a processargs has room for 62 besides the
    // Launcher's own two.
    if (auto const launcher = connect_or_fail(LAUNCHER); launcher != Uandle::Invalid) {
        expect("AddHandles with 63 handles",
               add_handles(launcher, job, TOO_MANY_HANDLES, ours::HandleType::User0), Status::Ok);
        if (auto const vmo = readable_self(); vmo != Uandle::Invalid) {
            expect("Launch after 63 handles", launch_status(launcher, vmo, job), Status::InvArgs);
        }
        auto _ = ours::handle_close(launcher);
    }

    // A malformed message, here eight bytes where a header takes sixteen:
    // the connection is closed with no epitaph, so there is nothing to read.
    if (auto const launcher = connect_or_fail(LAUNCHER); launcher != Uandle::Invalid) {
        std::array<u8, 8> garbage{};
        expect("a malformed message to the Launcher",
               write(launcher, {.bytes = garbage, .handles = {}}), Status::Ok);
        Incoming closed;
        expect("the Launcher's answer to a malformed message", closed.receive(launcher),
               Status::PeerClosed);
        auto _ = ours::handle_close(launcher);
    }
}

/// fsio::spawn() of this program with the argument `child`, which must
/// exit 0: the Launcher end to end, through the namespace this process was
/// handed, cloned (design D12).
auto check_spawn(Uandle const job) -> void {
    auto const argv = std::array{SELF_IN_BOOT, CHILD};
    auto const spawned = fsio::spawn(job, SELF_IN_BOOT, argv);
    if (!expect("spawn of routing_checks child", status_of(spawned), Status::Ok)) {
        return;
    }
    auto const process = spawned.unwrap();
    auto observed = ours::Signals::None;
    ours::InfoProcess info{};
    if (expect("waiting for the child",
               ours::object_wait_one(process, ours::Signals::Terminated, ours::INSTANT_INFINITE,
                                     &observed),
               Status::Ok) &&
        expect("the child's exit",
               ours::object_get_info(process, ours::InfoTopic::Process, &info, sizeof(info),
                                     nullptr, nullptr),
               Status::Ok) &&
        info.return_code != 0) {
        char code[21];
        auto _ = ort::debug_line({"routing_checks: the child exited with ",
                                  ort::signed_decimal(info.return_code, code)});
        fail("spawn of routing_checks child: a non-zero exit");
    }
    auto _ = ours::handle_close(process);
}

/// Sends |request| on |connection| and reads what comes back: Ok with the
/// reply in |reply|, an epitaph's status, or PeerClosed.
auto call(Uandle const connection, wire::Message const request, Incoming &reply) -> Status {
    // A write that finds the connection closed is not the answer: the
    // epitaph it was closed with, if any, is still there to read.
    if (auto const written = write(connection, request);
        written != Status::Ok && written != Status::PeerClosed) {
        return written;
    }
    if (auto const received = reply.receive(connection); received != Status::Ok) {
        return received;
    }
    if (auto const epitaph = reply.epitaph(); epitaph.is_some()) {
        return *epitaph;
    }
    return Status::Ok;
}

/// EchoString on |connection|: echo answers with what it was sent, through
/// the route root.cm offers from the sibling (D3 a).
auto check_echo_string(Uandle const connection) -> void {
    auto const txid = next_txid();
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const request =
        ours_examples_echo::encode_echo_string(bytes, {.txid = txid, .value = ECHO_TEXT});
    if (!expect("EchoString", status_of(request), Status::Ok)) {
        return;
    }
    Incoming reply;
    if (!expect("EchoString through the sibling's expose",
                call(connection, request.unwrap(), reply), Status::Ok)) {
        return;
    }
    auto const decoded = ours_examples_echo::decode_echo_string_reply(reply.message(), txid);
    if (expect("EchoString's reply", status_of(decoded), Status::Ok) &&
        expect("EchoString", decoded->status, Status::Ok) && decoded->response != ECHO_TEXT) {
        auto _ = ort::debug_line({"routing_checks: echo answered \"", decoded->response, "\""});
        fail("EchoString: the answer is not what was sent");
    }
}

/// Stop on |connection|: echo answers Ok and exits, which closes the
/// connection. Returns once it has, so that echo is dead.
auto stop_echo(Uandle const connection) -> bool {
    auto const txid = next_txid();
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    auto const request = ours_examples_echo::encode_stop(bytes, {.txid = txid});
    if (!expect("Stop", status_of(request), Status::Ok)) {
        return false;
    }
    Incoming reply;
    if (!expect("Stop", call(connection, request.unwrap(), reply), Status::Ok) ||
        !expect("Stop's reply",
                status_of(ours_examples_echo::decode_stop_reply(reply.message(), txid)),
                Status::Ok)) {
        return false;
    }
    // Its process closes every handle it held as it dies, this connection's
    // server end among them: nothing more comes on it.
    Incoming closed;
    return expect("echo's end of the connection after Stop", closed.receive(connection),
                  Status::PeerClosed);
}

/// What a new connection to |path| answers: the epitaph's status, or
/// PeerClosed when it closes with none. These checks expect an epitaph; a
/// message that is not one says so with a FAILED line.
auto connect_status(std::string_view const path) -> Status {
    auto const connection = connect_or_fail(path);
    if (connection == Uandle::Invalid) {
        return Status::BadState;
    }
    Incoming closed;
    auto status = closed.receive(connection);
    if (status == Status::Ok) {
        auto const epitaph = closed.epitaph();
        if (epitaph.is_none()) {
            fail("a connection expected to be refused answered something other than an epitaph");
            status = Status::BadState;
        } else {
            status = *epitaph;
        }
    }
    auto _ = ours::handle_close(connection);
    return status;
}

/// The sibling route, end to end: EchoString from echo through root.cm's
/// renaming offer and echo's expose, then Stop, then a route to echo that
/// answers NotFound, as one to a component that has exited does (D23).
auto check_echo() -> void {
    auto const connection = connect_or_fail(ECHO);
    if (connection == Uandle::Invalid) {
        return;
    }
    check_echo_string(connection);
    auto const stopped = stop_echo(connection);
    auto _ = ours::handle_close(connection);
    if (!stopped) {
        return;
    }

    // echo is dead, but the root may not know it yet. It learns from the
    // PEER_CLOSED on echo's outgoing directory, or the TERMINATED on its
    // process, each a packet on its port, in turn; this program learned from
    // the PEER_CLOSED on the connection above. All of them come from echo's
    // handle table being emptied as it dies, one handle at a time.
    //
    // Ours empties it in the order the handles were added
    // (HandleTable::clean(), kernel/object/handle_table.cpp): the outgoing
    // directory, a startup handle, before the connection, so the root's
    // packet is queued before the Open below and the first connect answers
    // NotFound. Were the table emptied newest first, the Open could reach
    // the root first, be
    // forwarded into echo's dying outgoing directory, and be lost with it:
    // PEER_CLOSED with no epitaph, the design's second worst interleaving.
    // Losing it takes that directory's close, which queues the root's
    // packet, so the connect after it answers NotFound. One such loss is
    // allowed, no more.
    auto answered = connect_status(ECHO);
    if (answered == Status::PeerClosed) {
        answered = connect_status(ECHO);
    }
    expect("a route to echo once it has exited", answered, Status::NotFound);
}

/// The copy check_spawn() starts: the namespace it was handed is a clone of
/// its parent's, read through once. Started by the shell instead (plan step
/// 15), it gets the shell's namespace, which has /boot too. /denied is
/// never among them: the parent's was closed, and spawn() hands on only
/// live entries.
auto run_child() -> int {
    if (!expect("fsio::install() in the child", fsio::install(), Status::Ok)) {
        return 1;
    }
    check_read(SELF_IN_BOOT);
    expect("/denied in the child", status_of(fsio::open("/denied", Flags::PermReadBytes)),
           Status::NotFound);
    if (failures != 0) {
        return 1;
    }
    return 0;
}
} // namespace

auto main(int argc, char **argv) -> int {
    if (argc == 2 && std::string_view{argv[1]} == CHILD) {
        return run_child();
    }
    if (!expect("fsio::install()", fsio::install(), Status::Ok)) {
        auto _ = ort::debug_line("routing_checks: FAILED");
        return 1;
    }
    if (auto const bound = fsio::installed_namespace().size(); bound != 5) {
        fail("the namespace does not hold the manifest's four directory uses and /svc");
    }
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    auto const job = ort::take_startup_handle(ours::HandleType::JobDefault, 0);

    check_read(SELF_IN_BOOT);
    check_read(SELF_IN_BOOT_R);
    check_read(SELF_IN_BIN);
    check_read_dirents("/boot", "bin", ours_io::DirentType::Directory);
    check_read_dirents("/boot", "meta", ours_io::DirentType::Directory);
    check_read_dirents("/bin", "routing_checks", ours_io::DirentType::File);
    check_read_dirents("/svc", "ours.process.Launcher", ours_io::DirentType::Service);
    if (vma == Uandle::Invalid) {
        fail("started without VmaRoot");
    } else {
        check_backing_memory(vma);
    }
    check_execute_refused();
    check_denied();
    check_debug_resource();
    check_unoffered();
    if (job == Uandle::Invalid) {
        fail("started without JobDefault");
    } else {
        check_launcher_refusals(job);
        check_spawn(job);
    }
    check_echo();

    if (failures != 0) {
        char count[10];
        auto _ = ort::debug_line(
            {"routing_checks: ", ort::decimal(failures, count), " check(s) FAILED"});
        return 1;
    }
    auto _ = ort::debug_line("routing_checks: passed");
    return 0;
}
