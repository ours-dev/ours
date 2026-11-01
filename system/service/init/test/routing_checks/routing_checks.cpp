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
/// Its manifest also uses protocols, which live in its /svc: the root's
/// built-ins ours.process.Launcher, ours.resource.Debug, Sampling, Irq, Ioport,
/// Mmio and Iommu, and ours.boot.Acpi, which root.cm offers, ours.acpi.Route
/// and ours.test.Echo, which root.cm offers from its siblings, and
/// ours.test.Unoffered, which nobody offers. It checks that each ours.resource Get hands out a
/// resource that does what it is for -- debug_read takes the Debug resource,
/// interrupt_create takes the root of the interrupt lines for a free line,
/// ioports_request takes the root of the I/O ports for a port the kernel
/// does not drive, and object_get_info says the Mmio resource is the root of
/// the physical address space -- that the unoffered protocol answers NotFound, that so
/// does a name its manifest does not use at all, that the Launcher refuses
/// what it cannot start -- a VMO without Execute, a reserved handle type,
/// too many handles, a malformed message -- and that fsio::spawn() refuses a
/// program /boot does not have. Then it spawns itself
/// with the argument `child` through fsio::spawn(), and expects that copy,
/// which only reads one file through the namespace it was handed, to exit 0.
///
/// ours.boot.Acpi, which root.cm offers too, hands out the machine's ACPI tables
/// as a read-only VMO. It checks that the handle cannot write, that the
/// directory in the VMO is well formed, and that every table in it has the
/// signature and length of its own header and adds up to zero.
///
/// ours.acpi.Route, which root.cm offers from the sibling acpi-route, answers
/// from the firmware's tables which GSI an ISA interrupt arrives on and where
/// a PCI segment's configuration space is. It checks that each GSI it gives
/// is a pin the kernel has a line for, that QEMU's q35 is wired as the
/// service says, that what has no route is refused, and that the window is
/// where QEMU's is. The kernel's own translation of an ISA number is not
/// compared with the service's any more (see check_acpi_route()).
///
/// The use of ours.test.Echo is the one route here that ends
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
#include <ours/acpi_tables.hpp>
#include <ours/interrupt.hpp>
#include <ours/object_info.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/sampler.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <cstring>

#include <fsio/fsio.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_acpi/client.hpp>
#include <ours_examples_echo/ours_examples_echo.hpp>
#include <ours_io/ours_io.hpp>
#include <ours_process/ours_process.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>
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
constexpr auto UNOFFERED = std::string_view{"/svc/ours.test.Unoffered"};
/// A name its manifest does not use at all, so its /svc has no entry for it.
constexpr auto UNDECLARED = std::string_view{"/svc/ours.test.Undeclared"};
/// echo's protocol, under the name root.cm's offer gives it.
constexpr auto ECHO = std::string_view{"/svc/ours.test.Echo"};

/// The pin whose line check_irq_resource() takes: GSI 3, COM2's, which is
/// free, since QEMU has no COM2 with one serial port and the kernel keeps only
/// its own lines.
constexpr auto GSI_FREE = u32{3};

/// The port check_ioport_resource() opens: 0x70, the RTC's index port, which
/// the kernel does not drive.
constexpr auto RTC_INDEX_PORT = u16{0x70};

/// What check_echo() sends echo, and expects back.
constexpr auto ECHO_TEXT = std::string_view{"routing_checks"};

/// The argument that makes it the spawned copy (run_child()).
constexpr auto CHILD = std::string_view{"child"};

/// One handle more than the Add* calls may give: a processargs message
/// carries CHANNEL_MAX_MSG_HANDLES, 64, and the Launcher adds three of its
/// own (launch::ProcessLauncherState::MAX_HANDLES).
constexpr auto TOO_MANY_HANDLES = usize{ours::CHANNEL_MAX_MSG_HANDLES} - 3 + 1;

/// How many checks failed.
constinit u32 FAILURES = 0;

/// The txid of the next two-way request this program writes by hand.
constinit u32 TXID_COUNTER = 1;

auto next_txid() -> u32 {
    auto const txid = TXID_COUNTER;
    TXID_COUNTER += 1;
    return txid;
}

auto status_name(Status const status) -> std::string_view {
    return ours::to_string(status);
}

/// Says that |what| answered |got| where |expected| was due, once, with
/// FAILED in the line.
auto fail(std::string_view const what, Status const got, Status const expected) -> void {
    FAILURES += 1;
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
    FAILURES += 1;
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

/// Directory.ReadDirents on |path|: "." comes first, as ioservice::Server lists a
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
    // EXECUTE gate of ioservice::Server's file Open.
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

/// ours_resource::get(): the resource |protocol| hands out, from /svc, which
/// the caller owns from here, or Invalid after saying why not -- the route's
/// epitaph, the root's failure, or a reply that does not decode.
auto get_resource(ours_resource::Protocol const &protocol) -> Uandle {
    auto const got = ours_resource::get(protocol);
    if (!expect(protocol.get.selector, status_of(got), Status::Ok)) {
        return Uandle::Invalid;
    }
    return got.unwrap();
}

/// Debug.Get answers with the Debug resource: a handle debug_read takes,
/// which it refuses from anything but the System Debug resource. debug_read
/// of 0 bytes returns at once.
auto check_debug_resource() -> void {
    auto const debug = get_resource(ours_resource::DEBUG_PROTOCOL);
    if (debug == Uandle::Invalid) {
        return;
    }
    auto actual = usize{0};
    expect("debug_read(the resource Debug.Get gave, 0 bytes)",
           ours::debug_read(debug, nullptr, 0, &actual), Status::Ok);
    auto _ = ours::handle_close(debug);
}

/// Sampling.Get answers with the Sampling resource, which sampler_create
/// takes and nothing else does. The check never takes the system's one
/// session, which the shell's profile check, a component started beside
/// this one, needs: it hands in a config with |reserved| set. sampler_create
/// checks the resource first, refusing any other one (BadType, OutOfRange),
/// then the config (InvArgs), and only then looks for a session, so InvArgs
/// says the resource passed and nothing was claimed.
auto check_sampling_resource() -> void {
    auto const sampling = get_resource(ours_resource::SAMPLING_PROTOCOL);
    if (sampling == Uandle::Invalid) {
        return;
    }
    auto const refused = ours::SamplerConfig{
        .period = ours::SAMPLER_MIN_PERIOD,
        .buffer_size = sizeof(ours::SamplerRecord),
        .targets = ours::SamplerTargets::Kernel,
        .reserved = 1,
    };
    auto session = Uandle::Invalid;
    expect("sampler_create(the resource Sampling.Get gave, reserved = 1)",
           ours::sampler_create(sampling, 0, &refused, &session), Status::InvArgs);
    auto _ = ours::handle_close(sampling);
}

/// interrupt_map gives the number interrupt_create takes for a pin the
/// platform names, to the holder of the interrupt root. On QEMU's q35, the
/// machine this image runs on, the timer is on GSI 2 (the firmware moves ISA 0
/// off GSI 0), COM2's pin is GSI 3, and the pins from GSI 16 up are PCI's.
/// What is checked:
///   - a line's number is not the pin's, and the pins that can be asked for
///     (GSI 0, 3 and 16) have numbers of their own, GSI 0 being the pin the
///     timer was moved off, a line of its own; the timer's number is derived
///     from GSI 3's and so cannot be compared with them;
///   - GSI 2 is the kernel's timer line, which no program can take:
///     interrupt_map refuses it (AccessDenied) and writes nothing. Its line
///     is the one before GSI 3's, since the kernel numbers lines
///     consecutively in the order of their pins, and interrupt_create
///     refuses that number too (AccessDenied);
///   - GSI 3 is one free line, taken with the number as given; and a PCI pin,
///     GSI 16, has a line of its own, which an interrupt object takes
///     level-triggered, as a PCI function's INTx is;
///   - the refusals: the invalid source is InvArgs, ahead of the handle; a
///     source with no pin is NoResource, 0x100 and 0x103 included, which as 8
///     bits would be GSI 0 and GSI 3; no handle is BadHandle. A refused call
///     writes nothing.
auto check_interrupt_map(Uandle const irq) -> void {
    constexpr auto GSI_TIMER = u32{2};
    constexpr auto GSI_PCI = u32{16};
    constexpr auto GSI_MOVED_OFF = u32{0};
    constexpr auto GSI_WRAPS_TO_0 = u32{0x100};
    constexpr auto GSI_WRAPS_TO_3 = u32{0x103};
    constexpr auto NO_PIN = u32{200};
    constexpr auto UNTOUCHED = u32{0xA5A5'A5A5};

    auto timer_refused = UNTOUCHED;
    auto free_line = UNTOUCHED;
    auto pci = UNTOUCHED;
    auto moved_off = UNTOUCHED;
    auto ok = true;
    ok &= expect("interrupt_map(Irq root, 2, the kernel's timer)",
                 ours::interrupt_map(irq, GSI_TIMER, &timer_refused), Status::AccessDenied);
    if (timer_refused != UNTOUCHED) {
        fail("interrupt_map: a refused call wrote its output");
    }
    ok &= expect("interrupt_map(Irq root, 3)", ours::interrupt_map(irq, GSI_FREE, &free_line),
                 Status::Ok);
    ok &=
        expect("interrupt_map(Irq root, 16)", ours::interrupt_map(irq, GSI_PCI, &pci), Status::Ok);
    ok &= expect("interrupt_map(Irq root, 0)", ours::interrupt_map(irq, GSI_MOVED_OFF, &moved_off),
                 Status::Ok);
    if (!ok) {
        return;
    }

    // GSI 2 is refused, so its line is found from its neighbour's: the kernel
    // numbers lines consecutively in the order of their pins (irq::Domain),
    // and GSI 2 and GSI 3 are both pins on this machine.
    auto const timer = free_line - 1;
    if (timer == GSI_TIMER || free_line == GSI_FREE || pci == GSI_PCI ||
        moved_off == GSI_MOVED_OFF) {
        fail("interrupt_map: a line's number is the pin's number");
    }
    if (free_line == pci || free_line == moved_off || pci == moved_off) {
        fail("interrupt_map: two pins have one number");
    }

    auto interrupt = Uandle::Invalid;
    expect("interrupt_create(the line before GSI 3's, the timer's)",
           ours::interrupt_create(irq, timer, ours::InterruptOptions::None, &interrupt),
           Status::AccessDenied);
    if (expect("interrupt_create(the number interrupt_map gave for GSI 3)",
               ours::interrupt_create(irq, free_line, ours::InterruptOptions::None, &interrupt),
               Status::Ok)) {
        expect("close(the interrupt on the number interrupt_map gave for GSI 3)",
               ours::handle_close(interrupt), Status::Ok);
    }
    interrupt = Uandle::Invalid;
    if (expect("interrupt_create(the number interrupt_map gave for GSI 16, ModeLevelLow)",
               ours::interrupt_create(irq, pci, ours::InterruptOptions::ModeLevelLow, &interrupt),
               Status::Ok)) {
        expect("close(the interrupt on the number interrupt_map gave for GSI 16)",
               ours::handle_close(interrupt), Status::Ok);
    }

    auto refused = UNTOUCHED;
    expect("interrupt_map(0xFFFFFFFF)", ours::interrupt_map(irq, 0xFFFF'FFFF, &refused),
           Status::InvArgs);
    expect("interrupt_map(no handle, 0xFFFFFFFF): the source is checked before the handle",
           ours::interrupt_map(Uandle::Invalid, 0xFFFF'FFFF, &refused), Status::InvArgs);
    // The source is 32 bits; as 8 bits these would be GSI 0 and GSI 3.
    expect("interrupt_map(0x100)", ours::interrupt_map(irq, GSI_WRAPS_TO_0, &refused),
           Status::NoResource);
    expect("interrupt_map(0x103)", ours::interrupt_map(irq, GSI_WRAPS_TO_3, &refused),
           Status::NoResource);
    expect("interrupt_map(200)", ours::interrupt_map(irq, NO_PIN, &refused), Status::NoResource);
    expect("interrupt_map(no handle, 3)", ours::interrupt_map(Uandle::Invalid, GSI_FREE, &refused),
           Status::BadHandle);
    if (refused != UNTOUCHED) {
        fail("interrupt_map wrote its output though it refused");
    }
}

/// Irq.Get answers with the root of the interrupt lines, which
/// interrupt_create takes for a physical line: an interrupt on the free GSI 3
/// is made with it, which takes the line, and closed, which lets the line go.
/// The I/O port root, a resource of another kind, would be refused with
/// BadType, so this also shows the root did not mix the two up. The same root
/// is what interrupt_map asks for.
auto check_irq_resource() -> void {
    auto const irq = get_resource(ours_resource::IRQ_PROTOCOL);
    if (irq == Uandle::Invalid) {
        return;
    }
    auto interrupt = Uandle::Invalid;
    auto line = u32{0};
    if (!expect("interrupt_map(the resource Irq.Get gave, 3)",
                ours::interrupt_map(irq, GSI_FREE, &line), Status::Ok)) {
        auto _ = ours::handle_close(irq);
        return;
    }
    if (expect("interrupt_create(the resource Irq.Get gave, GSI 3)",
               ours::interrupt_create(irq, line, ours::InterruptOptions::ModeDefault, &interrupt),
               Status::Ok)) {
        expect("close(the interrupt on GSI 3)", ours::handle_close(interrupt), Status::Ok);
    }
    check_interrupt_map(irq);
    auto _ = ours::handle_close(irq);
}

/// Ioport.Get answers with the root of the I/O ports, which ioports_request
/// takes: port 0x70 is opened to this process with it and closed again.
/// Nothing reaches the port in between; that a port is open is ioport.test's
/// and the shell's to check. The IRQ root would be refused with BadType, as
/// above.
auto check_ioport_resource() -> void {
    auto const ioport = get_resource(ours_resource::IOPORT_PROTOCOL);
    if (ioport == Uandle::Invalid) {
        return;
    }
    expect("ioports_request(the resource Ioport.Get gave, 0x70, 1)",
           ours::ioports_request(ioport, RTC_INDEX_PORT, 1), Status::Ok);
    expect("ioports_release(the resource Ioport.Get gave, 0x70, 1)",
           ours::ioports_release(ioport, RTC_INDEX_PORT, 1), Status::Ok);
    auto line = u32{0};
    expect("interrupt_map(the resource Ioport.Get gave, 200): the resource is checked first",
           ours::interrupt_map(ioport, 200, &line), Status::BadType);
    expect("interrupt_map(the resource Ioport.Get gave)",
           ours::interrupt_map(ioport, GSI_FREE, &line), Status::BadType);
    auto _ = ours::handle_close(ioport);
}

/// The physical address of the PCI Express configuration window (ECAM) on
/// QEMU's q35, the machine this image runs on: bus 0, device 0, function 0,
/// the host bridge, is its first 4 KiB. A driver reads where the window is from
/// ACPI's MCFG table; nothing here parses it, so this test knows the machine
/// and its firmware: the window is where the firmware puts it, and the image
/// boots under edk2, which puts it at 0xE0000000 (SeaBIOS uses 0xB0000000).
constexpr auto ECAM_BASE = ours::PhysAddr{0xE000'0000};

/// What the host bridge's vendor id register, the first 16 bits of its
/// configuration space, holds on QEMU's q35: Intel's.
constexpr auto INTEL_VENDOR_ID = u16{0x8086};

/// A page inside the RAM of any machine this image boots (the smallest has
/// 512 MiB), and the local APIC's registers, which the kernel drives itself.
/// No MmIo resource grants either.
constexpr auto SOME_RAM = ours::PhysAddr{0x1000'0000};
constexpr auto LOCAL_APIC = ours::PhysAddr{0xFEE0'0000};

/// vmo_create_physical with the resource Mmio.Get gave, the window it makes
/// and what that window refuses.
///
/// What no resource grants is refused whoever asks: RAM and the local APIC
/// come back AccessDenied from the root of the physical address space, where
/// the arguments that make no window come back InvArgs and OutOfRange, a
/// handle of another kind BadType and no handle BadHandle. Then the window onto
/// the host bridge's configuration space is mapped and its vendor id read:
/// the first time anything in user space has read a device's register. The
/// window is the hardware's, so vmo_read and a slice of it say NotSupported.
auto check_physical_vmo(Uandle const vma, Uandle const mmio) -> void {
    auto none = Uandle::Invalid;
    expect("vmo_create_physical(the MmIo root, RAM)",
           ours::vmo_create_physical(mmio, SOME_RAM, PAGE, &none), Status::AccessDenied);
    expect("vmo_create_physical(the MmIo root, the local APIC)",
           ours::vmo_create_physical(mmio, LOCAL_APIC, PAGE, &none), Status::AccessDenied);
    expect("vmo_create_physical(an unaligned base)",
           ours::vmo_create_physical(mmio, ECAM_BASE + 1, PAGE, &none), Status::InvArgs);
    expect("vmo_create_physical(a size of part of a page)",
           ours::vmo_create_physical(mmio, ECAM_BASE, PAGE + 1, &none), Status::InvArgs);
    expect("vmo_create_physical(a size of 0)", ours::vmo_create_physical(mmio, ECAM_BASE, 0, &none),
           Status::InvArgs);
    expect("vmo_create_physical(a window past the top of the space)",
           ours::vmo_create_physical(mmio, ~ours::PhysAddr{0} & ~(PAGE - 1), 2 * PAGE, &none),
           Status::OutOfRange);
    expect("vmo_create_physical(no resource)",
           ours::vmo_create_physical(Uandle::Invalid, ECAM_BASE, PAGE, &none), Status::BadHandle);
    if (auto const irq = get_resource(ours_resource::IRQ_PROTOCOL); irq != Uandle::Invalid) {
        expect("vmo_create_physical(the Irq root)",
               ours::vmo_create_physical(irq, ECAM_BASE, PAGE, &none), Status::BadType);
        auto _ = ours::handle_close(irq);
    }

    auto window = Uandle::Invalid;
    if (!expect("vmo_create_physical(the MmIo root, the host bridge's configuration space)",
                ours::vmo_create_physical(mmio, ECAM_BASE, PAGE, &window), Status::Ok)) {
        return;
    }

    auto size = u64{0};
    expect("vmo_get_size(the window)", ours::vmo_get_size(window, &size), Status::Ok);
    if (size != PAGE) {
        fail("the window onto one page is not one page");
    }

    // Mapped with no MapRange: a physical VMO is always populated, and a map
    // that left the window unfilled would die on the read below.
    auto address = ours::VirtAddr{0};
    if (expect("vma_map(the window, PermRead)",
               ours::vma_map(vma, ours::VmOptions::PermRead, 0, window, 0, PAGE, &address),
               Status::Ok)) {
        auto const vendor = *reinterpret_cast<volatile u16 const *>(address);
        if (vendor != INTEL_VENDOR_ID) {
            char text[18];
            auto _ = ort::debug_line(
                {"routing_checks: the host bridge's vendor id reads ", ort::hex(vendor, text)});
            fail("the vendor id of the host bridge, read through the window, is not Intel's");
        }
        expect("vma_unmap(the window)", ours::vma_unmap(vma, address, PAGE), Status::Ok);
    }

    std::array<u8, 4> bytes{};
    expect("vmo_read(the window)", ours::vmo_read(window, bytes.data(), 0, bytes.size()),
           Status::NotSupported);
    auto slice = Uandle::Invalid;
    expect("vmo_create_child(a slice of the window)",
           ours::vmo_create_child(window, ours::VmoChildOptions::Slice, 0, PAGE, &slice),
           Status::NotSupported);
    auto _ = ours::handle_close(window);
}

/// The calls that put memory within a device's reach, refused as far as they
/// can be without the System resource of the IOMMU slot, which this program is
/// not given yet: a handle that names nothing, a handle of another kind, and a
/// resource of another kind (the MmIo root). What a real IOMMU, BTI and pin
/// answer needs that slot, and is checked where it arrives.
///
/// pmt_unpin closes a handle that is a pin and nothing else, so the VMO it is
/// given here is still there afterwards, and is closed by this check.
auto check_device_dma_refusals(Uandle const mmio) -> void {
    auto vmo = Uandle::Invalid;
    if (!expect("vmo_create(a page)", ours::vmo_create(PAGE, ours::VmoOptions::None, &vmo),
                Status::Ok)) {
        return;
    }
    auto none = Uandle::Invalid;
    std::array<ours::PhysAddr, 1> addresses{};
    constexpr auto RW = ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite;

    expect("iommu_create(no resource)",
           ours::iommu_create(Uandle::Invalid, ours::IommuType::Stub, nullptr, 0, &none),
           Status::BadHandle);
    expect("iommu_create(a VMO)", ours::iommu_create(vmo, ours::IommuType::Stub, nullptr, 0, &none),
           Status::BadType);
    expect("iommu_create(the MmIo root)",
           ours::iommu_create(mmio, ours::IommuType::Stub, nullptr, 0, &none), Status::BadType);
    expect("bti_create(no IOMMU)", ours::bti_create(Uandle::Invalid, 0, 0, &none),
           Status::BadHandle);
    expect("bti_create(a VMO)", ours::bti_create(vmo, 0, 0, &none), Status::BadType);
    expect("bti_pin(no BTI)",
           ours::bti_pin(Uandle::Invalid, RW, vmo, 0, PAGE, addresses.data(), 1, &none),
           Status::BadHandle);
    expect("bti_pin(a VMO as the BTI)",
           ours::bti_pin(vmo, RW, vmo, 0, PAGE, addresses.data(), 1, &none), Status::BadType);
    expect("bti_release_quarantine(no BTI)", ours::bti_release_quarantine(Uandle::Invalid),
           Status::BadHandle);
    expect("bti_release_quarantine(a VMO)", ours::bti_release_quarantine(vmo), Status::BadType);
    expect("pmt_unpin(no handle)", ours::pmt_unpin(Uandle::Invalid), Status::BadHandle);
    if (none != Uandle::Invalid) {
        fail("a refused call wrote a handle");
    }

    expect("pmt_unpin(a VMO)", ours::pmt_unpin(vmo), Status::BadType);
    auto size = u64{0};
    expect("vmo_get_size(the VMO pmt_unpin was given)", ours::vmo_get_size(vmo, &size), Status::Ok);
    auto _ = ours::handle_close(vmo);
}

/// Mmio.Get answers with the root of the physical address space: an MmIo
/// resource of base 0 and size 0. The kernel is asked what the resource is,
/// which shows the root did not hand out the root of another kind in its place,
/// and then it is used: check_physical_vmo(), and as the wrong kind of resource
/// for the calls that want a System one: check_device_dma_refusals().
auto check_mmio_resource(Uandle const vma) -> void {
    auto const mmio = get_resource(ours_resource::MMIO_PROTOCOL);
    if (mmio == Uandle::Invalid) {
        return;
    }
    ours::InfoResource info{};
    auto actual = usize{0};
    auto available = usize{0};
    if (expect("object_get_info(the resource Mmio.Get gave)",
               ours::object_get_info(mmio, ours::InfoTopic::Resource, &info, sizeof(info), &actual,
                                     &available),
               Status::Ok)) {
        if (info.kind != ours::RsrcKind::MmIo || info.base != 0 || info.size != 0) {
            fail("the resource Mmio.Get gave is not the root of the physical address space");
        }
    }
    if (vma == Uandle::Invalid) {
        fail("started without VmaRoot, so the MmIo root has nothing to map into");
    } else {
        check_physical_vmo(vma, mmio);
    }
    check_device_dma_refusals(mmio);
    auto _ = ours::handle_close(mmio);
}

/// The pages the DMA checks pin, and what a physical address of one looks like.
constexpr auto DMA_PAGES = usize{4};

/// A second handle to |handle| with |rights|, or Invalid after saying why not.
auto narrowed(Uandle const handle, ours::Rights const rights) -> Uandle {
    auto copy = Uandle::Invalid;
    if (!expect("handle_duplicate(narrowed)", ours::handle_duplicate(handle, rights, &copy),
                Status::Ok)) {
        return Uandle::Invalid;
    }
    return copy;
}

/// Whether anything of |vmo| is pinned: a snapshot of a VMO with a pinned page
/// is refused (BadState), and made when nothing is. The child is closed.
auto is_pinned(Uandle const vmo) -> bool {
    auto child = Uandle::Invalid;
    auto const status =
        ours::vmo_create_child(vmo, ours::VmoChildOptions::SnapshotOnWrite, 0, PAGE, &child);
    if (status == Status::Ok) {
        auto _ = ours::handle_close(child);
        return false;
    }
    if (status != Status::BadState) {
        fail("vmo_create_child(a snapshot, to see whether it is pinned)", status, Status::BadState);
    }
    return true;
}

/// bti_pin with |bti|, and everything it refuses without pinning a page: the
/// options, the arguments, the count of addresses, the rights on the BTI and
/// on the VMO, and the order in which they are asked.
auto check_pin_refusals(Uandle const bti, Uandle const vmo) -> void {
    constexpr auto RW = ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite;
    std::array<ours::PhysAddr, 2 * DMA_PAGES> addresses{};
    auto none = Uandle::Invalid;
    auto const ask = [&](ours::BtiPinOptions const options, Uandle const from, u64 const offset,
                         u64 const size, usize const count) {
        return ours::bti_pin(bti, options, from, offset, size, addresses.data(), count, &none);
    };

    expect("bti_pin(no Perm bit)", ask(ours::BtiPinOptions::None, vmo, 0, PAGE, 1),
           Status::InvArgs);
    expect("bti_pin(Compress alone)", ask(ours::BtiPinOptions::Compress, vmo, 0, PAGE, 1),
           Status::InvArgs);
    expect("bti_pin(an option that means nothing)",
           ask(RW | static_cast<ours::BtiPinOptions>(1U << 9), vmo, 0, PAGE, 1), Status::InvArgs);
    expect(
        "bti_pin(Compress with Contiguous)",
        ask(RW | ours::BtiPinOptions::Compress | ours::BtiPinOptions::Contiguous, vmo, 0, PAGE, 1),
        Status::InvArgs);
    expect("bti_pin(an unaligned offset)", ask(RW, vmo, 1, PAGE, 1), Status::InvArgs);
    expect("bti_pin(a size of 0)", ask(RW, vmo, 0, 0, 0), Status::InvArgs);
    expect("bti_pin(a size of part of a page)", ask(RW, vmo, 0, PAGE + 1, 1), Status::InvArgs);
    expect("bti_pin(one address too few)", ask(RW, vmo, 0, DMA_PAGES * PAGE, DMA_PAGES - 1),
           Status::InvArgs);
    expect("bti_pin(one address too many)", ask(RW, vmo, 0, DMA_PAGES * PAGE, DMA_PAGES + 1),
           Status::InvArgs);
    expect("bti_pin(a range past the end)", ask(RW, vmo, 0, (DMA_PAGES + 1) * PAGE, DMA_PAGES + 1),
           Status::OutOfRange);
    expect("bti_pin(a range that starts at the end)", ask(RW, vmo, DMA_PAGES * PAGE, PAGE, 1),
           Status::OutOfRange);

    // The size bounds the call, not only the count: Contiguous is one address
    // for any size.
    auto huge = Uandle::Invalid;
    if (expect(
            "vmo_create(one page more than a pin may be)",
            ours::vmo_create((ours::BTI_PIN_MAX_PAGES + 1) * PAGE, ours::VmoOptions::None, &huge),
            Status::Ok)) {
        expect("bti_pin(Contiguous over more pages than a pin may be)",
               ask(RW | ours::BtiPinOptions::Contiguous, huge, 0,
                   (ours::BTI_PIN_MAX_PAGES + 1) * PAGE, 1),
               Status::InvArgs);
        auto _ = ours::handle_close(huge);
    }

    // Rights: what the device may do is limited by what this program may.
    auto const read_only = narrowed(vmo, ours::Rights::Read | ours::Rights::Map);
    auto const no_map = narrowed(vmo, ours::Rights::Read | ours::Rights::Write);
    auto const bti_no_map = narrowed(bti, ours::Rights::Write);
    if (read_only != Uandle::Invalid) {
        expect("bti_pin(PermWrite over a VMO that cannot be written)",
               ask(RW, read_only, 0, PAGE, 1), Status::AccessDenied);
        expect("bti_pin(PermWrite alone over a VMO that cannot be written)",
               ask(ours::BtiPinOptions::PermWrite, read_only, 0, PAGE, 1), Status::AccessDenied);
        auto _ = ours::handle_close(read_only);
    }
    if (no_map != Uandle::Invalid) {
        expect("bti_pin(a VMO without Map)", ask(RW, no_map, 0, PAGE, 1), Status::AccessDenied);
        auto _ = ours::handle_close(no_map);
    }
    if (bti_no_map != Uandle::Invalid) {
        expect("bti_pin(a BTI without Map)",
               ours::bti_pin(bti_no_map, RW, vmo, 0, PAGE, addresses.data(), 1, &none),
               Status::AccessDenied);
        // The BTI is looked at first: its answer is the one given when the VMO
        // handle is bad as well.
        expect("bti_pin(a BTI without Map, and no VMO)",
               ours::bti_pin(bti_no_map, RW, Uandle::Invalid, 0, PAGE, addresses.data(), 1, &none),
               Status::AccessDenied);
        auto _ = ours::handle_close(bti_no_map);
    }
    expect("bti_pin(no VMO)", ask(RW, Uandle::Invalid, 0, PAGE, 1), Status::BadHandle);
    expect("bti_pin(a BTI where the VMO is wanted)", ask(RW, bti, 0, PAGE, 1), Status::BadType);

    if (none != Uandle::Invalid) {
        fail("a refused bti_pin wrote a handle");
    }
    if (is_pinned(vmo)) {
        fail("a refused bti_pin left a page pinned");
    }
}

/// The addresses a pin hands over are the pages' own: distinct, whole pages,
/// and the same again for a second pin of the same pages, which do not move.
/// Compress is the same here as no option; Contiguous is one address, and for
/// four pages it is Ok exactly when the four are one run.
auto check_pin_addresses(Uandle const bti, Uandle const vmo) -> void {
    constexpr auto RW = ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite;
    std::array<ours::PhysAddr, DMA_PAGES> first{};
    std::array<ours::PhysAddr, DMA_PAGES> again{};
    std::array<ours::PhysAddr, DMA_PAGES> compressed{};
    auto pin_a = Uandle::Invalid;
    auto pin_b = Uandle::Invalid;
    auto pin_c = Uandle::Invalid;
    if (!expect("bti_pin(four pages)",
                ours::bti_pin(bti, RW, vmo, 0, DMA_PAGES * PAGE, first.data(), DMA_PAGES, &pin_a),
                Status::Ok)) {
        return;
    }

    auto sound = true;
    for (auto at = usize{0}; at < DMA_PAGES; ++at) {
        sound = sound && first[at] != 0 && first[at] % PAGE == 0;
        for (auto other = at + 1; other < DMA_PAGES; ++other) {
            sound = sound && first[at] != first[other];
        }
    }
    if (!sound) {
        fail("the addresses of a pin are not four distinct whole pages");
    }
    if (!is_pinned(vmo)) {
        fail("a pin left its pages unpinned");
    }

    if (expect("bti_pin(the same four pages again)",
               ours::bti_pin(bti, RW, vmo, 0, DMA_PAGES * PAGE, again.data(), DMA_PAGES, &pin_b),
               Status::Ok) &&
        again != first) {
        fail("a second pin of the same pages gave other addresses");
    }
    if (expect("bti_pin(Compress)",
               ours::bti_pin(bti, RW | ours::BtiPinOptions::Compress, vmo, 0, DMA_PAGES * PAGE,
                             compressed.data(), DMA_PAGES, &pin_c),
               Status::Ok) &&
        compressed != first) {
        fail("Compress gave other addresses than none, though a page is the smallest run");
    }

    // Four pages as one address: the same as the first of the four when they
    // are one run, and BadState when they are not.
    auto one = std::array<ours::PhysAddr, 1>{};
    auto contiguous_pin = Uandle::Invalid;
    auto one_run = true;
    for (auto at = usize{1}; at < DMA_PAGES; ++at) {
        one_run = one_run && first[at] == first[0] + at * PAGE;
    }
    auto const contiguous = ours::bti_pin(bti, RW | ours::BtiPinOptions::Contiguous, vmo, 0,
                                          DMA_PAGES * PAGE, one.data(), 1, &contiguous_pin);
    if (one_run) {
        if (expect("bti_pin(Contiguous, four pages that are one run)", contiguous, Status::Ok) &&
            one[0] != first[0]) {
            fail("Contiguous gave an address that is not the first page's");
        }
    } else {
        expect("bti_pin(Contiguous, four pages that are not one run)", contiguous,
               Status::BadState);
    }
    auto single = Uandle::Invalid;
    if (expect("bti_pin(Contiguous, one page)",
               ours::bti_pin(bti, RW | ours::BtiPinOptions::Contiguous, vmo, 0, PAGE, one.data(), 1,
                             &single),
               Status::Ok) &&
        one[0] != first[0]) {
        fail("Contiguous over one page gave an address that is not that page's");
    }

    // Each pin ends once. A handle is closed by its unpin, so a second unpin
    // of it finds nothing.
    for (auto const pin: {pin_a, pin_b, pin_c, contiguous_pin, single}) {
        if (pin != Uandle::Invalid) {
            expect("pmt_unpin", ours::pmt_unpin(pin), Status::Ok);
        }
    }
    expect("pmt_unpin(a pin already unpinned)", ours::pmt_unpin(pin_a), Status::BadHandle);
    if (is_pinned(vmo)) {
        fail("every pin was unpinned and a page is still pinned");
    }
}

/// A pin whose last handle is closed with no unpin is quarantined: its page
/// stays pinned, the BTI answers BadState to every pin after it, and
/// bti_release_quarantine, which needs Write, lets it go. This is what happens
/// to a driver that dies with its device running, minus the dying.
auto check_quarantine(Uandle const bti) -> void {
    constexpr auto RW = ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite;
    auto vmo = Uandle::Invalid;
    if (!expect("vmo_create(a page)", ours::vmo_create(PAGE, ours::VmoOptions::None, &vmo),
                Status::Ok)) {
        return;
    }
    std::array<ours::PhysAddr, 1> address{};
    auto lost = Uandle::Invalid;
    auto refused = Uandle::Invalid;
    if (expect("bti_pin(a page, to lose)",
               ours::bti_pin(bti, RW, vmo, 0, PAGE, address.data(), 1, &lost), Status::Ok)) {
        expect("handle_close(a pin, with no unpin)", ours::handle_close(lost), Status::Ok);
    }
    if (!is_pinned(vmo)) {
        fail("a pin whose last handle was closed let its page go");
    }
    expect("bti_pin(a BTI with a quarantine)",
           ours::bti_pin(bti, RW, vmo, 0, PAGE, address.data(), 1, &refused), Status::BadState);

    auto const bti_no_write = narrowed(bti, ours::Rights::Map);
    if (bti_no_write != Uandle::Invalid) {
        expect("bti_release_quarantine(a BTI without Write)",
               ours::bti_release_quarantine(bti_no_write), Status::AccessDenied);
        auto _ = ours::handle_close(bti_no_write);
    }
    if (!is_pinned(vmo)) {
        fail("a refused bti_release_quarantine let the page go");
    }

    expect("bti_release_quarantine", ours::bti_release_quarantine(bti), Status::Ok);
    expect("bti_release_quarantine(nothing left)", ours::bti_release_quarantine(bti), Status::Ok);
    auto again = Uandle::Invalid;
    if (expect("bti_pin(once the quarantine is released)",
               ours::bti_pin(bti, RW, vmo, 0, PAGE, address.data(), 1, &again), Status::Ok)) {
        expect("pmt_unpin", ours::pmt_unpin(again), Status::Ok);
    }
    if (is_pinned(vmo)) {
        fail("the quarantine was released and the page is still pinned");
    }
    auto _ = ours::handle_close(vmo);
}

/// vmo_create_contiguous with what it refuses, then a VMO of three pages and
/// one on 64 KiB: pinned page by page, their addresses follow one another; and
/// pinned with Contiguous, which is Ok for this memory where it is BadState
/// for a VMO that merely happens to be scattered, they are one address.
auto check_contiguous_vmo(Uandle const bti) -> void {
    constexpr auto RW = ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite;
    auto none = Uandle::Invalid;
    // The largest block of the build this image boots with, in pages; the call
    // says 4 MiB.
    constexpr auto MOST = usize{1} << 10;

    expect("vmo_create_contiguous(no BTI)",
           ours::vmo_create_contiguous(Uandle::Invalid, PAGE, 0, &none), Status::BadHandle);
    auto plain = Uandle::Invalid;
    if (expect("vmo_create(a page)", ours::vmo_create(PAGE, ours::VmoOptions::None, &plain),
               Status::Ok)) {
        expect("vmo_create_contiguous(a VMO as the BTI)",
               ours::vmo_create_contiguous(plain, PAGE, 0, &none), Status::BadType);
        auto _ = ours::handle_close(plain);
    }
    if (auto const bti_no_map = narrowed(bti, ours::Rights::Write); bti_no_map != Uandle::Invalid) {
        expect("vmo_create_contiguous(a BTI without Map)",
               ours::vmo_create_contiguous(bti_no_map, PAGE, 0, &none), Status::AccessDenied);
        auto _ = ours::handle_close(bti_no_map);
    }
    expect("vmo_create_contiguous(a size of 0)", ours::vmo_create_contiguous(bti, 0, 0, &none),
           Status::InvArgs);
    expect("vmo_create_contiguous(an alignment below a page)",
           ours::vmo_create_contiguous(bti, PAGE, 5, &none), Status::InvArgs);
    expect("vmo_create_contiguous(an alignment that is not a shift)",
           ours::vmo_create_contiguous(bti, PAGE, 64, &none), Status::InvArgs);
    expect("vmo_create_contiguous(more than the largest block)",
           ours::vmo_create_contiguous(bti, (2 * MOST) * PAGE, 0, &none), Status::NoMemory);
    expect("vmo_create_contiguous(an alignment past the largest block)",
           ours::vmo_create_contiguous(bti, PAGE, 12 + 11, &none), Status::NoMemory);
    if (none != Uandle::Invalid) {
        fail("a refused vmo_create_contiguous wrote a handle");
    }

    auto vmo = Uandle::Invalid;
    if (expect("vmo_create_contiguous(three pages)",
               ours::vmo_create_contiguous(bti, 3 * PAGE, 0, &vmo), Status::Ok)) {
        auto size = u64{0};
        expect("vmo_get_size(the contiguous VMO)", ours::vmo_get_size(vmo, &size), Status::Ok);
        if (size != 3 * PAGE) {
            fail("a contiguous VMO of three pages is not three pages");
        }

        std::array<u8, 64> bytes{};
        bytes.fill(0xFF);
        expect("vmo_read(the start of a contiguous VMO)",
               ours::vmo_read(vmo, bytes.data(), 0, bytes.size()), Status::Ok);
        auto zero = true;
        for (auto const byte: bytes) {
            zero = zero && byte == 0;
        }
        bytes.fill(0xFF);
        expect("vmo_read(the end of a contiguous VMO)",
               ours::vmo_read(vmo, bytes.data(), 3 * PAGE - bytes.size(), bytes.size()),
               Status::Ok);
        for (auto const byte: bytes) {
            zero = zero && byte == 0;
        }
        if (!zero) {
            fail("a contiguous VMO is not zero");
        }

        std::array<ours::PhysAddr, 3> pages{};
        std::array<ours::PhysAddr, 1> one{};
        auto by_page = Uandle::Invalid;
        auto whole = Uandle::Invalid;
        if (expect("bti_pin(the contiguous VMO, page by page)",
                   ours::bti_pin(bti, RW, vmo, 0, 3 * PAGE, pages.data(), 3, &by_page),
                   Status::Ok)) {
            if (pages[1] != pages[0] + PAGE || pages[2] != pages[0] + 2 * PAGE) {
                fail("the pages of a contiguous VMO do not follow one another");
            }
            expect("pmt_unpin(page by page)", ours::pmt_unpin(by_page), Status::Ok);
        } else {
            // Nothing to compare the next pin's address with.
            pages[0] = ~ours::PhysAddr{0};
        }
        if (expect("bti_pin(the contiguous VMO, Contiguous)",
                   ours::bti_pin(bti, RW | ours::BtiPinOptions::Contiguous, vmo, 0, 3 * PAGE,
                                 one.data(), 1, &whole),
                   Status::Ok)) {
            if (one[0] != pages[0]) {
                fail("the one address of a contiguous VMO is not its first page's");
            }
            expect("pmt_unpin(Contiguous)", ours::pmt_unpin(whole), Status::Ok);
        }
        auto _ = ours::handle_close(vmo);
    }

    constexpr auto SIXTY_FOUR_KIB = usize{1} << 16;
    auto aligned = Uandle::Invalid;
    if (expect("vmo_create_contiguous(two pages on 64 KiB)",
               ours::vmo_create_contiguous(bti, 2 * PAGE, 16, &aligned), Status::Ok)) {
        std::array<ours::PhysAddr, 1> one{};
        auto pin = Uandle::Invalid;
        if (expect("bti_pin(the aligned VMO, Contiguous)",
                   ours::bti_pin(bti, RW | ours::BtiPinOptions::Contiguous, aligned, 0, 2 * PAGE,
                                 one.data(), 1, &pin),
                   Status::Ok)) {
            if (one[0] % SIXTY_FOUR_KIB != 0) {
                fail("a contiguous VMO asked to start on 64 KiB does not");
            }
            expect("pmt_unpin(the aligned VMO)", ours::pmt_unpin(pin), Status::Ok);
        }
        auto _ = ours::handle_close(aligned);
    }
}

/// iommu_create and bti_create with what they refuse, then the checks of the
/// pins on the BTI they make.
auto check_iommu_and_bti(Uandle const resource) -> void {
    auto none = Uandle::Invalid;
    std::array<u8, 8> descriptor{};
    expect("iommu_create(a type nothing implements)",
           ours::iommu_create(resource, static_cast<ours::IommuType>(7), nullptr, 0, &none),
           Status::NotSupported);
    expect("iommu_create(a descriptor, for a stub that has no hardware)",
           ours::iommu_create(resource, ours::IommuType::Stub, descriptor.data(), descriptor.size(),
                              &none),
           Status::InvArgs);
    expect("iommu_create(a descriptor past the limit)",
           ours::iommu_create(resource, ours::IommuType::Stub, nullptr,
                              ours::IOMMU_DESC_MAX_SIZE + 1, &none),
           Status::InvArgs);
    if (auto const debug = get_resource(ours_resource::DEBUG_PROTOCOL); debug != Uandle::Invalid) {
        expect("iommu_create(the System resource of another slot)",
               ours::iommu_create(debug, ours::IommuType::Stub, nullptr, 0, &none),
               Status::OutOfRange);
        auto _ = ours::handle_close(debug);
    }
    if (none != Uandle::Invalid) {
        fail("a refused iommu_create wrote a handle");
    }

    auto iommu = Uandle::Invalid;
    if (!expect("iommu_create(Stub)",
                ours::iommu_create(resource, ours::IommuType::Stub, nullptr, 0, &iommu),
                Status::Ok)) {
        return;
    }
    expect("bti_create(an option nothing defines)", ours::bti_create(iommu, 1, 0x1234, &none),
           Status::InvArgs);
    auto bti = Uandle::Invalid;
    if (expect("bti_create", ours::bti_create(iommu, 0, 0x1234, &bti), Status::Ok)) {
        auto vmo = Uandle::Invalid;
        if (expect("vmo_create(four pages)",
                   ours::vmo_create(DMA_PAGES * PAGE, ours::VmoOptions::None, &vmo), Status::Ok)) {
            check_pin_refusals(bti, vmo);
            check_pin_addresses(bti, vmo);
            auto _ = ours::handle_close(vmo);
        }
        check_quarantine(bti);
        check_contiguous_vmo(bti);
        auto _ = ours::handle_close(bti);
    }
    auto _ = ours::handle_close(iommu);
}

/// Iommu.Get answers with the System resource of the IOMMU slot: a resource of
/// that kind, at that base, of size 1. It is then used as a driver would: to
/// make an IOMMU and a BTI, and pin memory through them.
auto check_iommu_resource() -> void {
    auto const resource = get_resource(ours_resource::IOMMU_PROTOCOL);
    if (resource == Uandle::Invalid) {
        return;
    }
    ours::InfoResource info{};
    auto actual = usize{0};
    auto available = usize{0};
    if (expect("object_get_info(the resource Iommu.Get gave)",
               ours::object_get_info(resource, ours::InfoTopic::Resource, &info, sizeof(info),
                                     &actual, &available),
               Status::Ok)) {
        if (info.kind != ours::RsrcKind::System || info.base != ours::RSRC_SYSTEM_IOMMU_BASE ||
            info.size != 1) {
            fail("the resource Iommu.Get gave is not the System IOMMU slot");
            return;
        }
    }
    check_iommu_and_bti(resource);
    auto _ = ours::handle_close(resource);
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

/// A protocol its manifest does not use at all: its /svc has no entry of that
/// name, so the root's /svc refuses the Open itself with NotFound, before any
/// route is walked (check_unoffered() is the other NotFound: an entry that
/// exists, whose route ends at an offer nobody made). The root says so on its
/// console, `init: routing_checks's /svc refuses an open of
/// "ours.test.Undeclared": NotFound`; that line is why this check is here.
auto check_undeclared() -> void {
    expect("a protocol the manifest does not use", connect_status(UNDECLARED), Status::NotFound);
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
    // directory, a startup handle, before the connection, so the root's packet
    // is queued before the Open below and the first connect answers NotFound.
    // Were the table emptied newest first, the Open could reach the root
    // first, be forwarded into echo's dying outgoing directory, and be lost
    // with it: PEER_CLOSED with no epitaph, the design's second worst
    // interleaving. Losing it takes that directory's close, which queues the
    // root's packet, so the connect after it answers NotFound. One such loss
    // is allowed, no more.
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
/// This process's handle to itself, which its starter handed over as
/// ProcSelf -- the root's runner for the component, the Launcher for its
/// spawned copy -- and the runtime took before main: object_get_info
/// answers for a started process through it.
auto check_process_self(std::string_view const who) -> void {
    ours::InfoProcess info{};
    auto const status = ours::object_get_info(ort::process_self(), ours::InfoTopic::Process, &info,
                                              sizeof(info), nullptr, nullptr);
    if (!expect(who, status, Status::Ok)) {
        return;
    }
    if (!(info.flags & ours::InfoProcessFlags::Started)) {
        fail(who);
    }
}

auto run_child() -> int {
    if (!expect("fsio::install() in the child", fsio::install(), Status::Ok)) {
        return 1;
    }
    check_process_self("object_get_info(process_self()) in the child");
    check_read(SELF_IN_BOOT);
    expect("/denied in the child", status_of(fsio::open("/denied", Flags::PermReadBytes)),
           Status::NotFound);
    if (FAILURES != 0) {
        return 1;
    }
    return 0;
}

/// The most entries the ACPI directory is checked for. A q35 machine has about
/// ten tables.
constexpr auto ACPI_MAX_ENTRIES = usize{64};

/// An ACPI signature as the directory spells it: the first character in the
/// low byte.
constexpr auto acpi_signature(char const (&name)[5]) -> u32 {
    return u32(u8(name[0])) | u32(u8(name[1])) << 8 | u32(u8(name[2])) << 16 |
           u32(u8(name[3])) << 24;
}

/// Whether the |length| bytes of |vmo| at |offset| add up to zero modulo 256,
/// which is how ACPI checksums a table. Read in pieces: a table can be larger
/// than anything worth keeping on this stack. Says why and answers false if a
/// read fails.
auto acpi_bytes_sum_to_zero(Uandle const vmo, u64 const offset, u32 const length) -> bool {
    auto sum = u8{0};
    auto done = u32{0};
    while (done < length) {
        std::array<u8, 256> piece{};
        auto const n = std::min<usize>(piece.size(), length - done);
        if (!expect("vmo_read(an ACPI table)", ours::vmo_read(vmo, piece.data(), offset + done, n),
                    Status::Ok)) {
            return false;
        }
        for (auto i = usize{0}; i < n; ++i) {
            sum = u8(sum + piece[i]);
        }
        done += u32(n);
    }
    return sum == 0;
}

/// Acpi.Get answers with the machine's ACPI tables: a VMO that is read and
/// mapped and cannot be written, holding the directory of
/// <ours/acpi_tables.hpp> and then the firmware's tables, the DSDT first. The
/// directory is read as the route service will read it: every entry lies
/// inside the VMO, on an 8 byte boundary and after the entry before it; its
/// signature and length are those of the table's own header; and the table's
/// bytes add up to zero, which is ACPI's checksum. QEMU's q35 has a FADT and a
/// MADT, so those two are asked for by name.
auto check_acpi_tables() -> void {
    auto const vmo = get_resource(ours_resource::ACPI_PROTOCOL);
    if (vmo == Uandle::Invalid) {
        return;
    }

    ours::InfoHandleBasic basic{};
    auto actual = usize{0};
    auto available = usize{0};
    if (expect("object_get_info(the VMO Acpi.Get gave)",
               ours::object_get_info(vmo, ours::InfoTopic::HandleBasic, &basic, sizeof(basic),
                                     &actual, &available),
               Status::Ok)) {
        if ((basic.rights & ours::Rights::Read) == ours::Rights::None ||
            (basic.rights & ours::Rights::Map) == ours::Rights::None) {
            fail("the VMO Acpi.Get gave cannot be read or mapped");
        }
        if ((basic.rights & ours::Rights::Write) != ours::Rights::None) {
            fail("the VMO Acpi.Get gave can be written");
        }
    }
    auto const byte = u8{0};
    expect("vmo_write(the ACPI tables)", ours::vmo_write(vmo, &byte, 0, 1), Status::AccessDenied);

    auto size = u64{0};
    if (!expect("vmo_get_size(the ACPI tables)", ours::vmo_get_size(vmo, &size), Status::Ok)) {
        auto _ = ours::handle_close(vmo);
        return;
    }
    ours::AcpiDirectory directory{};
    if (size < sizeof(directory) || size % PAGE != 0 ||
        !expect("vmo_read(the ACPI directory)",
                ours::vmo_read(vmo, &directory, 0, sizeof(directory)), Status::Ok)) {
        fail("the ACPI tables' VMO is not a whole number of pages, or cannot be read");
        auto _ = ours::handle_close(vmo);
        return;
    }
    if (directory.magic != ours::ACPI_DIRECTORY_MAGIC ||
        directory.version != ours::ACPI_DIRECTORY_VERSION) {
        fail("the ACPI directory has the wrong magic or version");
        auto _ = ours::handle_close(vmo);
        return;
    }
    auto const entries_end =
        sizeof(directory) + usize{directory.count} * sizeof(ours::AcpiDirectoryEntry);
    if (directory.count == 0 || directory.count > ACPI_MAX_ENTRIES || directory.size > size ||
        entries_end > directory.size) {
        fail("the ACPI directory's count or size is out of range");
        auto _ = ours::handle_close(vmo);
        return;
    }

    std::array<ours::AcpiDirectoryEntry, ACPI_MAX_ENTRIES> entries{};
    if (!expect("vmo_read(the ACPI entries)",
                ours::vmo_read(vmo, entries.data(), sizeof(directory),
                               usize{directory.count} * sizeof(ours::AcpiDirectoryEntry)),
                Status::Ok)) {
        auto _ = ours::handle_close(vmo);
        return;
    }

    auto found_fadt = false;
    auto found_madt = false;
    auto previous_end =
        (entries_end + ours::ACPI_TABLE_ALIGNMENT - 1) & ~(ours::ACPI_TABLE_ALIGNMENT - 1);
    for (auto index = usize{0}; index < directory.count; ++index) {
        auto const &entry = entries[index];
        if (entry.reserved != 0 || entry.offset % ours::ACPI_TABLE_ALIGNMENT != 0 ||
            entry.length < 36 || entry.offset < previous_end ||
            u64{entry.offset} + entry.length > directory.size) {
            fail("an entry of the ACPI directory is out of place");
            break;
        }
        previous_end = usize{entry.offset} + entry.length;

        // The table's own header says what the entry says: signature at 0,
        // length at 4.
        std::array<u8, 36> header{};
        if (!expect("vmo_read(an ACPI table's header)",
                    ours::vmo_read(vmo, header.data(), entry.offset, header.size()), Status::Ok)) {
            break;
        }
        auto signature = u32{0};
        auto length = u32{0};
        std::memcpy(&signature, header.data(), sizeof(signature));
        std::memcpy(&length, header.data() + 4, sizeof(length));
        if (signature != entry.signature || length != entry.length) {
            fail("an ACPI table's header disagrees with its directory entry");
        }
        if (!acpi_bytes_sum_to_zero(vmo, entry.offset, entry.length)) {
            fail("an ACPI table in the directory does not add up to zero");
        }
        found_fadt = found_fadt || entry.signature == acpi_signature("FACP");
        found_madt = found_madt || entry.signature == acpi_signature("APIC");
    }
    if (entries[0].signature != acpi_signature("DSDT")) {
        fail("the first table in the ACPI directory is not the DSDT");
    }
    if (!found_fadt || !found_madt) {
        fail("the ACPI directory has no FADT or no MADT");
    }
    auto _ = ours::handle_close(vmo);
}

/// Route.IsaIrq and Route.EcamBase answer from the firmware's tables in user
/// space.
///
/// The kernel reads the same tables for its own translation of an ISA number
/// (platform::isa_irq_line()), but no system call exposes it: interrupt_map
/// takes the platform's number for a pin and not an ISA number. So the
/// service is not compared with the kernel's translation here, but with what
/// QEMU's q35 tables say, which the kernel prints when it programs the pins
/// (the table "ISA Irq | Remapped | Trigger mode | Polarity | Global IRQ" of
/// the boot log): the timer, ISA 0, is moved to GSI 2; the lines the PCI link
/// devices use, ISA 5, 9, 10 and 11, are level triggered and active high on
/// their own GSIs; every other line is its own GSI, edge triggered and active
/// high. What is checked:
///   - for each ISA number 0 to 15 but the cascade, the service answers the
///     GSI, trigger and polarity of that table, and the GSI is a pin the
///     kernel has a line for (interrupt_map does not answer NoResource). The
///     table is machine-specific, as ISA 0 on GSI 2 is for the checks of
///     interrupt_map, and a different machine needs a different table;
///   - the service's GSI for ISA 0 is the kernel's timer line, which
///     interrupt_map refuses (AccessDenied): the one place the kernel's own
///     binding shows it, and no independent check, for it restates GSI 2;
///   - the cascade, ISA 2, a number past the ISA lines and 0xFFFFFFFF are no
///     ISA line: NotFound;
///   - the service's ECAM window for segment 0 begins at the address this
///     program already maps QEMU's window at, and no other segment is listed.
/// The kernel's own translation is checked against its own table by
/// pinlines.test, which a person runs from the kernel shell and
/// boot_check.py does not.
auto check_acpi_route() -> void {
    constexpr auto NUM_ISA_IRQS = u32{16};
    constexpr auto CASCADE = u32{2};
    constexpr auto ISA_TIMER = u32{0};
    constexpr auto GSI_TIMER = u32{2};
    constexpr auto UNTOUCHED = u32{0xA5A5'A5A5};

    auto const irq = get_resource(ours_resource::IRQ_PROTOCOL);
    if (irq == Uandle::Invalid) {
        return;
    }

    for (auto isa = u32{0}; isa < NUM_ISA_IRQS; ++isa) {
        auto const route = ours_acpi::isa_irq(isa);
        if (isa == CASCADE) {
            expect("Route.IsaIrq(2), the cascade", status_of(route), Status::NotFound);
            continue;
        }
        if (!expect("Route.IsaIrq(an ISA number)", status_of(route), Status::Ok)) {
            continue;
        }

        // QEMU's table, as the kernel logs it.
        auto const moved = isa == ISA_TIMER;
        auto const level = isa == 5 || isa == 9 || isa == 10 || isa == 11;
        if (route->gsi != (moved ? GSI_TIMER : isa)) {
            fail("Route.IsaIrq: an ISA number is not on the GSI QEMU's q35 puts it on");
        }
        if (route->trigger != (level ? ours_acpi::Trigger::Level : ours_acpi::Trigger::Edge)) {
            fail("Route.IsaIrq: an ISA number does not signal as QEMU's q35 says (trigger)");
        }
        if (route->polarity != ours_acpi::Polarity::ActiveHigh) {
            fail("Route.IsaIrq: an ISA number does not signal as QEMU's q35 says (polarity)");
        }

        // A pin the kernel has a line for answers Ok, or AccessDenied if the
        // kernel holds that line itself, as it does the timer's.
        auto line = UNTOUCHED;
        auto wanted = Status::Ok;
        if (moved) {
            wanted = Status::AccessDenied;
        }
        expect("interrupt_map(the service's GSI for an ISA number)",
               ours::interrupt_map(irq, route->gsi, &line), wanted);
    }

    // Not ISA lines: no route. 0xffffffff is the number interrupt_map keeps for
    // itself and answers InvArgs; to the service it is just a number that is no
    // ISA line.
    for (auto const isa: {NUM_ISA_IRQS, u32{0x100}, u32{0xffff'ffff}}) {
        expect("Route.IsaIrq(past the ISA lines)", status_of(ours_acpi::isa_irq(isa)),
               Status::NotFound);
    }

    auto const window = ours_acpi::ecam_base(0);
    if (expect("Route.EcamBase(0)", window.is_ok() ? Status::Ok : window.unwrap_err(),
               Status::Ok)) {
        if (window->address != ECAM_BASE) {
            fail("Route.EcamBase(0) is not where QEMU's configuration space is");
        }
        if (window->last_bus < window->first_bus ||
            window->size != (u64{window->last_bus} - window->first_bus + 1) * (u64{1} << 20)) {
            fail("Route.EcamBase(0): the size is not one MiB to a bus");
        }
    }
    auto const none = ours_acpi::ecam_base(1);
    expect("Route.EcamBase(1), a segment the MCFG does not list",
           none.is_ok() ? Status::Ok : none.unwrap_err(), Status::NotFound);
    auto _ = ours::handle_close(irq);
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
    check_process_self("object_get_info(process_self())");
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
    check_irq_resource();
    check_ioport_resource();
    check_mmio_resource(vma);
    check_iommu_resource();
    check_acpi_tables();
    check_acpi_route();
    check_sampling_resource();
    check_unoffered();
    check_undeclared();
    if (job == Uandle::Invalid) {
        fail("started without JobDefault");
    } else {
        check_launcher_refusals(job);
        check_spawn(job);
    }
    check_echo();

    if (FAILURES != 0) {
        char count[10];
        auto _ = ort::debug_line(
            {"routing_checks: ", ort::decimal(FAILURES, count), " check(s) FAILED"});
        return 1;
    }
    auto _ = ort::debug_line("routing_checks: passed");
    return 0;
}
