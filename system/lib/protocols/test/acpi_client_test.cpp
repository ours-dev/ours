/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::acpi's client against a fake Route provider: the typed client's
// calls on a connection the caller keeps, and what each failure reads as.
// The test plays the provider inside the fixture's pump -- when the
// client's wait asks the machine to progress, the pump reads the request
// off the server end, decodes it with the codec's own decoders, and answers
// per the mode with the codec's reply encoders. What the cases pin: both
// round trips (the ISA line and the segment arriving as sent, the answer's
// fields coming back), each method's failure status form arriving as
// itself, the epitaph a provider closes with standing in for the failure,
// an Ok epitaph reading as the connection closing, an epitaph whose status
// field names no Status reading InvArgs, a reply carrying a handle refused
// with its handle closed, and a reply to the other method caught as
// InvArgs, in both directions. The /svc overloads run too: a fake /svc --
// a Tree served by a LoopServer whose service Open hands the channel to
// this suite's pump -- is bound into the process's real namespace, and the
// one-arg isa_irq and ecam_base walk it, on the happy path and on a
// missing entry (the refusal arrives as the server's epitaph) and with
// nothing bound at all (the connect itself refuses), either method.
#include <prot/acpi_client.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "fake_kernel.hpp"
#include <fsio/fsio.hpp>
#include <gtest/gtest.h>
#include <ioservice/loop_server.hpp>
#include <loop/loop.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

// The /svc overloads resolve through the process's namespace, whose library
// does not link on the host -- src/fsio.cpp is user-toolchain-only, taking
// startup handles from the runtime. What links here is the namespace itself
// (//system/lib/fsio:namespace: bind, unbind, resolve, clean_path), and
// these two definitions front it as fsio.cpp's namespace open does: the
// path cleaned, the mount point found in the installed namespace, the Open
// written one-way. install() never ran on the host, so the namespace is
// empty and a case binds its fake /svc into it directly.
namespace {
auto installed() -> fsio::Namespace & {
    static fsio::Namespace INSTALLED;
    return INSTALLED;
}
} // namespace

auto fsio::installed_namespace() -> Namespace & {
    return installed();
}

auto fsio::connect(std::string_view const path, ours::Uandle const server) -> ours::Status {
    // |server| is consumed whatever the answer, as the channel write
    // consumes it; a refusal before the write is the caller's to close.
    fsio::PathBuffer clean;
    if (auto const status = fsio::clean_path(path, clean); status != ours::Status::Ok) {
        auto _ = ours::handle_close(server);
        return status;
    }
    auto const resolved = installed().resolve(clean);
    if (resolved.is_err()) {
        auto _ = ours::handle_close(server);
        return resolved.unwrap_err();
    }
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<ours::Uandle, 1> handles{};
    auto const encoded = prot::io::encode_open(
        bytes, handles,
        prot::io::OpenRequest{
            .flags = prot::io::Flags::ProtocolService, .path = resolved->rest, .object = server});
    if (encoded.is_err()) {
        return encoded.unwrap_err();
    }
    auto const message = encoded.unwrap();
    return ours::channel_write(resolved->directory, ours::ChannelWriteOptions::None,
                               message.bytes.data(), static_cast<u32>(message.bytes.size()),
                               message.handles.data(), static_cast<u32>(message.handles.size()));
}

namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

// The wait pumps the machine while it waits: the fake kernel's progress is
// this hook, which the fixture points at the provider's answering logic.
static std::function<void()> WAIT_PUMP = [] {};

// The vDSO declaration carries the asm label; the label rides the
// declarator, not the trailing type (the extern "C" spelling would land a
// platform underscore the label's reference never carries).
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline,
                                  ours::Signals *observed) __asm__("ours_object_wait_one");
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline, ours::Signals *observed) {
    for (auto spin = u32{0}; spin < 1'000'000; ++spin) {
        WAIT_PUMP();
        // A queued message wins over the peer's death: a survivor holding a
        // last message reads it first, the epitaph case lives on this order.
        if (fake::kernel().queued_messages(handle) != 0 &&
            (signals & ours::Signals::Readable) != ours::Signals::None) {
            *observed = ours::Signals::Readable;
            return ours::Status::Ok;
        }
        if (fake::kernel().peer_closed(handle)) {
            *observed = ours::Signals::PeerClosed;
            return ours::Status::Ok;
        }
    }
    *observed = ours::Signals::None;
    return ours::Status::TimeOut;
}

namespace {
using ours::Status;
using ours::Uandle;

/// The question each round trip sends, and the answer the provider gives:
/// the facts the test and the pump share.
constexpr auto ISA_LINE = u32{4};
constexpr auto ISA_DOMAIN = u32{0};
constexpr auto ISA_GSI = u32{21};
constexpr auto ECAM_SEGMENT = u16{1};
constexpr auto ECAM_ADDRESS = u64{0xE000'0000};
constexpr auto ECAM_FIRST_BUS = u8{0};
constexpr auto ECAM_LAST_BUS = u8{31};
constexpr auto ECAM_SIZE = u64{0x0200'0000};

/// The status field of the garbage epitaph: not a value ours::Status names.
constexpr auto GARBAGE_STATUS = u32{0x9999};

/// One test's provider: the pump reads the request off the server end and
/// answers per the mode. IsaIrq and EcamBase answer the question; IsaIrqFailure
/// and EcamBaseFailure answer with the status form, no body; StrayHandle
/// rides a handle on the reply, which this family never carries; IsaIrqAsEcam
/// and EcamAsIsaIrq answer one question with the other method's reply;
/// Epitaph closes with the epitaph after reading the request; OkEpitaph
/// closes saying nothing; GarbageEpitaph closes with an epitaph whose status
/// no Status names; Silent closes without answering.
enum class Mode {
    IsaIrq,
    EcamBase,
    StrayHandle,
    IsaIrqAsEcam,
    EcamAsIsaIrq,
    IsaIrqFailure,
    EcamBaseFailure,
    Epitaph,
    OkEpitaph,
    GarbageEpitaph,
    Silent,
};

/// A channel pair, the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// The path the fake /svc is bound at, the value its service node is added
/// with, and a name the /svc overloads never ask for -- what the miss case
/// binds instead of the route, whose walk then fails.
constexpr auto SVC_PATH = std::string_view{"/svc"};
constexpr auto SVC_SERVICE = u64{1};
constexpr auto UNHELD_NAME = std::string_view{"ours.acpi.Other"};

/// The connections the fake /svc serves at once.
constexpr auto SVC_CONNECTIONS = usize{4};

/// What the fake /svc's server keeps, far past what a stack frame carries.
struct SvcStorage {
    std::array<ioservice::Node, 4> nodes;
    std::array<ioservice::Connection, SVC_CONNECTIONS> connections;
    std::array<ioservice::LoopServer::Binding, SVC_CONNECTIONS> bindings;
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    ioservice::ReplyBuffer reply;
};

/// The fake /svc's delegate: an Open of the service node hands the
/// request's channel to this suite's provider, which answers the question
/// on it as the running case's pump. The channel lands where the pump
/// finds it.
class SvcDelegate final: public ioservice::LoopServer::Delegate {
  public:
    auto connect(u64 const service, Uandle const channel) -> void override {
        EXPECT_EQ(service, SVC_SERVICE);
        connected_ = channel;
    }

    Uandle connected_ = Uandle::Invalid;
};

/// Whether /svc is bound in the process's namespace just now: the
/// namespace is the process's, one across every case of this binary, so a
/// case that wants it bound for itself takes an earlier case's out first.
auto SVC_BOUND = false;

/// The fixture: a fresh fake kernel, a pair, the pump pointed at the mode.
class OursAcpiClient: public testing::Test {
  protected:
    OursAcpiClient() {
        fake::kernel().reset();
        ends_ = channel();
        m_provider = ends_[1];
        WAIT_PUMP = [this] { pump(); };
    }

    ~OursAcpiClient() override {
        WAIT_PUMP = [] {};
    }

    /// Serves a fake /svc holding the one service node |name| and binds it
    /// into the process's namespace, whose /svc overloads then walk it. A
    /// /svc an earlier case bound comes out first: a binding cannot be
    /// replaced, and what backed it did not survive the fresh fake kernel.
    auto bind_fake_svc(std::string_view const name) -> void {
        unbind_fake_svc();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);
        m_svc_slots = std::make_unique<std::array<loop::Slot, 8>>();
        m_svc_storage = std::make_unique<SvcStorage>();
        m_svc_loop.emplace(port, *m_svc_slots);
        m_svc_tree.emplace(std::span{m_svc_storage->nodes});
        auto const root = m_svc_tree->add_root().unwrap();
        auto _ = m_svc_tree->add_service(root, name, SVC_SERVICE).unwrap();
        m_svc_delegate = std::make_unique<SvcDelegate>();
        m_svc_server.emplace(*m_svc_tree, *m_svc_loop, syscalls(), memory(), m_svc_delegate.get());
        auto const ends = channel();
        EXPECT_EQ(m_svc_server->serve(ends[1], root, prot::io::R_STAR_DIR), Status::Ok);
        EXPECT_EQ(fsio::installed_namespace().bind(SVC_PATH, ends[0]), Status::Ok);
        SVC_BOUND = true;
    }

    /// Takes a /svc an earlier case bound back out of the namespace, so the
    /// case under way answers through nothing bound. The handle handed back
    /// may name nothing -- the fresh fake kernel forgot it -- so its close
    /// is best-effort, nothing asserted.
    auto unbind_fake_svc() -> void {
        if (!SVC_BOUND) {
            return;
        }
        auto const unbound = fsio::installed_namespace().unbind(SVC_PATH);
        ASSERT_TRUE(unbound.is_ok());
        auto _ = ours::handle_close(*unbound);
        SVC_BOUND = false;
    }

    /// One pump step: answer whatever is queued on the provider's end, per
    /// the mode. The client's calls spin this while they wait. The fake
    /// /svc's Loop runs first when there is one: that is what walks an Open
    /// to the service node between the spins, and the walk hands its
    /// channel to this pump to answer.
    auto pump() -> void {
        if (m_svc_loop.has_value()) {
            EXPECT_EQ(m_svc_loop->run(0), Status::TimeOut);
            // The walk hands its channel over only when it reaches the
            // service node; on a refused Open nothing is connected, and the
            // provider's end stays the fixture's own.
            if (m_svc_delegate->connected_ != Uandle::Invalid) {
                m_provider = m_svc_delegate->connected_;
            }
        }
        if (answered_) {
            return;
        }
        if (fake::kernel().queued_messages(m_provider) == 0) {
            return;
        }
        answered_ = true;
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        EXPECT_EQ(ours::channel_read(m_provider, ours::ChannelReadOptions::None, bytes.data(),
                                     handles.data(), static_cast<u32>(bytes.size()),
                                     static_cast<u32>(handles.size()), &num_bytes, &num_handles),
                  Status::Ok);
        auto const message = wire::Message{.bytes = std::span{bytes}.first(num_bytes),
                                           .handles = std::span{handles}.first(num_handles)};
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        auto const txid = header->txid;

        switch (mode_) {
            case Mode::IsaIrq: {
                auto const decoded = prot::acpi::decode_isa_irq(message);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->isa_irq, ISA_LINE);
                auto const reply = prot::acpi::encode_isa_irq_reply(
                    reply_scratch_, txid,
                    {.status = Status::Ok,
                     .domain = ISA_DOMAIN,
                     .gsi = ISA_GSI,
                     .trigger = prot::acpi::Trigger::Edge,
                     .polarity = prot::acpi::Polarity::ActiveHigh});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::EcamBase: {
                auto const decoded = prot::acpi::decode_ecam_base(message);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->segment, ECAM_SEGMENT);
                auto const reply = prot::acpi::encode_ecam_base_reply(reply_scratch_, txid,
                                                                      {.status = Status::Ok,
                                                                       .address = ECAM_ADDRESS,
                                                                       .size = ECAM_SIZE,
                                                                       .first_bus = ECAM_FIRST_BUS,
                                                                       .last_bus = ECAM_LAST_BUS});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::StrayHandle: {
                // A reply with a handle on it, which this family never
                // carries: the client's to close, and the answer reads as
                // InvArgs, not as the reply.
                auto const stray = channel();
                stray_ = stray[0];
                auto const reply = prot::acpi::encode_isa_irq_reply(
                    reply_scratch_, txid, {.status = Status::Ok, .gsi = ISA_GSI});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply, stray[0]);
                return;
            }
            case Mode::IsaIrqAsEcam: {
                // A well-formed reply, but to the other method: the
                // ordinal is what tells them apart, and the mismatch is
                // what the client names.
                auto const reply = prot::acpi::encode_ecam_base_reply(
                    reply_scratch_, txid,
                    {.status = Status::Ok, .address = ECAM_ADDRESS, .size = ECAM_SIZE});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::EcamAsIsaIrq: {
                // The same mismatch the other way round: an IsaIrq reply
                // where an EcamBase one was asked for.
                auto const reply = prot::acpi::encode_isa_irq_reply(
                    reply_scratch_, txid, {.status = Status::Ok, .gsi = ISA_GSI});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::IsaIrqFailure: {
                // The tables have no such line: the status form, no body.
                auto const reply = prot::acpi::encode_isa_irq_reply(reply_scratch_, txid,
                                                                    {.status = Status::NotFound});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::EcamBaseFailure: {
                // The tables have no such segment: the status form, no body.
                auto const reply = prot::acpi::encode_ecam_base_reply(reply_scratch_, txid,
                                                                      {.status = Status::NotFound});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::Epitaph: {
                auto epitaph = wire::encode_epitaph(reply_scratch_, Status::NotFound);
                ASSERT_TRUE(epitaph.is_ok());
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
            }
            case Mode::OkEpitaph: {
                auto epitaph = wire::encode_epitaph(reply_scratch_, Status::Ok);
                ASSERT_TRUE(epitaph.is_ok());
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
            }
            case Mode::GarbageEpitaph: {
                // The epitaph's frame, its status field replaced by one no
                // Status names: a closing that says nothing the client can
                // read as a reason.
                auto epitaph = wire::encode_epitaph(reply_scratch_, Status::NotFound);
                ASSERT_TRUE(epitaph.is_ok());
                wire::store(reply_scratch_, wire::STATUS_OFFSET, GARBAGE_STATUS);
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
            }
            case Mode::Silent:
                // No answer: just the peer's death.
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
        }
    }

    /// Writes an encoded reply onto the provider's end, back to the client,
    /// with |handle| riding it when the mode brings one.
    auto write_reply(wire::Message const &reply, Uandle const handle = Uandle::Invalid) -> void {
        auto riding = std::array<Uandle, 1>{handle};
        if (handle == Uandle::Invalid) {
            EXPECT_EQ(ours::channel_write(m_provider, ours::ChannelWriteOptions::None,
                                          reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                          nullptr, 0),
                      Status::Ok);
            return;
        }
        EXPECT_EQ(ours::channel_write(m_provider, ours::ChannelWriteOptions::None,
                                      reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                      riding.data(), 1),
                  Status::Ok);
    }

    /// The system calls the fake /svc's server makes, the host's own.
    static auto syscalls() -> ioservice::Syscalls {
        return {.handle_duplicate = ours::handle_duplicate};
    }

    /// What the fake /svc's server reads its messages into.
    auto memory() -> ioservice::LoopServer::Memory {
        return {.connections = m_svc_storage->connections,
                .bindings = m_svc_storage->bindings,
                .request_bytes = m_svc_storage->request_bytes,
                .request_handles = m_svc_storage->request_handles,
                .reply = &m_svc_storage->reply};
    }

    std::array<Uandle, 2> ends_{};
    /// The channel this case's provider answers on: the fixture's own
    /// server end, or what the fake /svc's walk handed over.
    Uandle m_provider = Uandle::Invalid;
    Mode mode_ = Mode::IsaIrq;
    bool answered_ = false;
    Uandle stray_ = Uandle::Invalid;
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_scratch_{};

    /// The fake /svc's wiring, for the cases that walk it. The storages are
    /// on the heap: together they are far past what a stack frame carries.
    std::unique_ptr<std::array<loop::Slot, 8>> m_svc_slots;
    std::unique_ptr<SvcStorage> m_svc_storage;
    std::optional<ioservice::Tree> m_svc_tree;
    std::optional<loop::Loop> m_svc_loop;
    std::optional<ioservice::LoopServer> m_svc_server;
    std::unique_ptr<SvcDelegate> m_svc_delegate;
};

} // namespace

/// The ISA round trip: the line arrives as sent, and the answer's GSI and
/// signalling come back in the caller's Result.
TEST_F(OursAcpiClient, IsaIrqRoundTrip) {
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_ok());
    EXPECT_EQ(routed->domain, ISA_DOMAIN);
    EXPECT_EQ(routed->gsi, ISA_GSI);
    EXPECT_EQ(routed->trigger, prot::acpi::Trigger::Edge);
    EXPECT_EQ(routed->polarity, prot::acpi::Polarity::ActiveHigh);
}

/// The tables have no such line: the provider's status form arrives as
/// itself, no body with it.
TEST_F(OursAcpiClient, IsaIrqFailureStatusArrivesAsItself) {
    mode_ = Mode::IsaIrqFailure;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::NotFound);
}

/// The ECAM round trip: the segment arrives as sent, and the window's
/// address, size and buses come back.
TEST_F(OursAcpiClient, EcamBaseRoundTrip) {
    mode_ = Mode::EcamBase;
    auto const window = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(window.is_ok());
    EXPECT_EQ(window->address, ECAM_ADDRESS);
    EXPECT_EQ(window->size, ECAM_SIZE);
    EXPECT_EQ(window->first_bus, ECAM_FIRST_BUS);
    EXPECT_EQ(window->last_bus, ECAM_LAST_BUS);
}

/// The tables have no such segment: the provider's status form arrives as
/// itself, no window with it.
TEST_F(OursAcpiClient, EcamBaseFailureStatusArrivesAsItself) {
    mode_ = Mode::EcamBaseFailure;
    auto const window = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(window.is_err());
    EXPECT_EQ(window.unwrap_err(), Status::NotFound);
}

/// A reply carrying a handle, which this family never does: the handle is
/// closed, and the answer reads InvArgs rather than the reply.
TEST_F(OursAcpiClient, StrayHandleReadsInvArgsAndIsClosed) {
    mode_ = Mode::StrayHandle;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::InvArgs);
    EXPECT_FALSE(fake::kernel().is_open(stray_));
}

/// A well-formed reply to the other method: the ordinal mismatch is caught
/// and named InvArgs, not read as the answer.
TEST_F(OursAcpiClient, OtherMethodsReplyReadsInvArgs) {
    mode_ = Mode::IsaIrqAsEcam;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::InvArgs);
}

/// The same mismatch the other way round: an IsaIrq reply where an
/// EcamBase one was asked for reads InvArgs too.
TEST_F(OursAcpiClient, OtherMethodsReplyToEcamBaseReadsInvArgs) {
    mode_ = Mode::EcamAsIsaIrq;
    auto const window = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(window.is_err());
    EXPECT_EQ(window.unwrap_err(), Status::InvArgs);
}

/// The epitaph a provider closes with stands in for the failure.
TEST_F(OursAcpiClient, EpitaphBecomesTheFailure) {
    mode_ = Mode::Epitaph;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::NotFound);
}

/// An Ok epitaph says nothing about why: it reads as the connection closing.
TEST_F(OursAcpiClient, OkEpitaphReadsPeerClosed) {
    mode_ = Mode::OkEpitaph;
    auto const routed = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::PeerClosed);
}

/// An epitaph whose status field names no Status: the closing carries no
/// reason the client can read as one, and that reads InvArgs.
TEST_F(OursAcpiClient, GarbageEpitaphReadsInvArgs) {
    mode_ = Mode::GarbageEpitaph;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::InvArgs);
}

/// A peer gone without a word reads PeerClosed.
TEST_F(OursAcpiClient, SilentCloseReadsPeerClosed) {
    mode_ = Mode::Silent;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::PeerClosed);
}

/// The /svc overload's ISA round trip: the one-arg isa_irq connects
/// through the process's namespace -- the path resolved under /svc, the
/// Open walking the fake tree to the service node, whose walk hands the
/// channel to this suite's provider -- and the answer's fields come back.
TEST_F(OursAcpiClient, NamespacedIsaIrqWalksTheFakeSvc) {
    bind_fake_svc(prot::acpi::ROUTE_NAME);
    auto const routed = prot::acpi::isa_irq(ISA_LINE);
    ASSERT_TRUE(routed.is_ok());
    EXPECT_EQ(routed->domain, ISA_DOMAIN);
    EXPECT_EQ(routed->gsi, ISA_GSI);
    EXPECT_EQ(routed->trigger, prot::acpi::Trigger::Edge);
    EXPECT_EQ(routed->polarity, prot::acpi::Polarity::ActiveHigh);
}

/// The /svc overload's ECAM round trip: the one-arg ecam_base connects
/// through the namespace the same way, and the window comes back.
TEST_F(OursAcpiClient, NamespacedEcamBaseWalksTheFakeSvc) {
    bind_fake_svc(prot::acpi::ROUTE_NAME);
    mode_ = Mode::EcamBase;
    auto const window = prot::acpi::ecam_base(ECAM_SEGMENT);
    ASSERT_TRUE(window.is_ok());
    EXPECT_EQ(window->address, ECAM_ADDRESS);
    EXPECT_EQ(window->size, ECAM_SIZE);
    EXPECT_EQ(window->first_bus, ECAM_FIRST_BUS);
    EXPECT_EQ(window->last_bus, ECAM_LAST_BUS);
}

/// The /svc overload on a /svc that does not hold the route: the walk
/// fails on the server's side, and the epitaph its refusal closes the
/// connection with is what the call reads.
TEST_F(OursAcpiClient, NamespacedMissReadsTheRefusal) {
    bind_fake_svc(UNHELD_NAME);
    auto const routed = prot::acpi::isa_irq(ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::NotFound);
}

/// The /svc overload with nothing bound under /svc: the namespace's
/// resolve refuses the connect itself, and that refusal is the call's.
TEST_F(OursAcpiClient, UnboundNamespaceRefusesTheConnect) {
    unbind_fake_svc();
    auto const window = prot::acpi::ecam_base(ECAM_SEGMENT);
    ASSERT_TRUE(window.is_err());
    EXPECT_EQ(window.unwrap_err(), Status::NotFound);
}

/// The same on the one-arg isa_irq: its connect through the namespace is
/// refused the same way when nothing backs /svc.
TEST_F(OursAcpiClient, UnboundNamespaceRefusesIsaIrqConnect) {
    unbind_fake_svc();
    auto const routed = prot::acpi::isa_irq(ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::NotFound);
}
