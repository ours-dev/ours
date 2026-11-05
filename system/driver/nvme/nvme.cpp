/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// nvme -- the userspace driver of QEMU's NVMe controller (storage-layer
/// plan, system/driver/nvme; the PCI hot-plug round made it resident), as a
/// component that serves ours.block.Block: a client opens the protocol, asks
/// for the namespace's geometry or for blocks by number, and the driver
/// drives the controller on its behalf. Root.cm offers it under the name
/// ours.block.Nvme, and its control under ours.nvme.Control.
///
/// Resident, not scan-once: a boot without the device finds no class-01h/08h
/// function and stays up anyway, serving ours.nvme.Control. A rescan -- the
/// boot's own first one, or a Control.Rescan a program asked for -- walks
/// the ECAM window through system/lib/pci, claims the first function of the
/// class that is not already running, sizes and assigns its BAR0 if it
/// arrived with no base (hot-plug: firmware assigns BARs at boot only),
/// brings the controller up through system/lib/nvme's device core, and
/// publishes the disk into the registry's tree. The device is found by class
/// code (01h/08h, mass storage / NVM controller) rather than by vendor and
/// device: the class names the family, and every vendor's controller is
/// driven the same way.
///
/// The objects:
///   - the MmIo root resource and the IOMMU System resource, through /svc:
///     the window mapping and the pin that hands the queues to the device;
///     the IOMMU resource is kept for the driver's life, because every
///     rescan that claims a function makes a fresh BTI for it;
///   - the DMA region: one contiguous run of pages from
///     vmo_create_contiguous, hand-sliced by system/lib/nvme's map -- the
///     admin queues, the Identify page, the I/O queues, and one page per
///     slot for the blocks, which is what PRP wants;
///   - the Loop, on which the outgoing directory, every client connection
///     and the control's connections wait;
///   - the server, block_server::BlockServer over the controller: the
///     library owns the connections, the decode, the slot table, and the
///     reply; the driver's start() builds the 64-byte command, writes it to
///     the I/O submission queue, rings the doorbell, and polls the
///     completion queue's phase bit until this command's entry lands --
///     the poll is on the same thread as everything else, so requests
///     serialize, which one I/O queue makes true anyway. The device's
///     interrupt pin is never unmasked: polling needs no line, and the
///     kernel's MSI does not exist yet;
///   - the control, a Handler whose connections answer Control.Rescan: the
///     request runs the rescan, and the reply carries its answer -- Ok when
///     a function was claimed, NotFound for an unchanged machine.
///
/// The events: the root routes a client's Open into the outgoing directory
/// (design D11); the server takes the channel and parks the request in a
/// slot; start() builds the command, submits, polls the completion, and
/// complete() turns it into the reply. A rescan's bring-up runs inside the
/// control's on_signals(), on the Loop thread: nothing else runs under it,
/// which is what makes the one-at-a-time controller safe to re-enter.
/// Before the driver leaves, CC.EN is cleared by the kernel's teardown of
/// what it pinned; the device's own shutdown is the DMA teardown order the
/// DmaRegion destructor keeps.
#include <ours/bti.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/time.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <cstdint>
#include <cstring>

#include <block_server/block_server.hpp>
#include <ddk/ddk.hpp>
#include <fsio/fsio.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <nvme/nvme.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <prot/acpi_client.hpp>
#include <prot/block.hpp>
#include <prot/dev.hpp>
#include <prot/dev_client.hpp>
#include <prot/nvme_control.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The block service's shape: two clients at once, and the device works on
/// one request at a time -- the poll model, MAX_INFLIGHT the tag depth the
/// one data page answers.
constexpr auto MAX_CLIENTS = usize{2};
constexpr auto MAX_INFLIGHT = usize{1};

/// How many Control.Rescan connections may be open at once: the shell's
/// rescan is one request on one connection of its own, and a second open
/// before the first's reply lands is refused with its channel closed.
constexpr auto CONTROL_CONNECTIONS = usize{4};

/// The address range a rescan assigns new BARs from, behind the ECAM
/// window: 64 MiB, far more than the disks this machine will hot-add.
constexpr u64 ARENA_BYTES = 64 * 1024 * 1024;

/// The block service over the polled controller, the driver's subclass of
/// the block-driver library: GetInfo answers the Identify'd geometry, and
/// start() runs one parked request to completion -- the 64-byte command
/// built, the data page primed for a Write, the doorbell rung, the
/// completion polled, the slot completed -- so the slot is free again by
/// the time start() returns. One queue pair and one data page make the
/// serialization physical. Until a rescan claims a controller there is
/// none, and start() answers NotFound: the protocol is served, the disk is
/// not there yet.
class NvmeBlockServer final
    : public block_server::BlockServer<NvmeBlockServer, MAX_CLIENTS, MAX_INFLIGHT> {
  public:
    using Base = block_server::BlockServer<NvmeBlockServer, MAX_CLIENTS, MAX_INFLIGHT>;
    using Storage = typename Base::Storage;

    NvmeBlockServer(Storage &storage, loop::Loop &loop) : Base(storage, loop) {}

    auto set_device(nvme::Device &device) -> void {
        m_device = &device;
    }

    /// GetInfo's answer: the namespace's logical block is the 512-byte
    /// sector ours.block names, and the count is what Identify returned.
    [[nodiscard]]
    auto geometry() const -> block_server::Geometry {
        return {.block_size = prot::block::BLOCK_SIZE, .blocks = m_blocks};
    }

    auto set_blocks(u64 const blocks) -> void {
        m_blocks = blocks;
    }

    /// The library's hand-off: run the parked request against the device,
    /// and complete it before returning. |data| is a Write's blocks, copied
    /// into the data page the command's PRP1 names -- the one copy of a
    /// Write.
    auto start(usize const index, std::span<u8 const> const data) -> Status {
        if (m_device == nullptr) {
            return Status::NotFound;
        }
        auto const &parked = slot(index);
        auto entry = nvme::SqEntry{};
        entry.opcode = data.empty() ? nvme::IO_READ : nvme::IO_WRITE;
        entry.nsid = nvme::NSID;
        entry.prp1 = std::to_underlying(m_device->data_phys());
        // SLBA, the first block, is 64 bits over CDW10 and CDW11; NLB, the
        // count, is zero-based.
        entry.cdw10 = static_cast<u32>(parked.lba);
        entry.cdw11 = static_cast<u32>(parked.lba >> 32);
        entry.cdw12 = parked.count - 1;
        if (!data.empty()) {
            std::memcpy(reinterpret_cast<void *>(m_device->data()), data.data(), data.size());
        }
        auto cqe = nvme::CqEntry{};
        if (!m_device->io().submit(entry, static_cast<u16>(index), cqe)) {
            return Status::TimeOut;
        }
        // Success is the whole status field clear -- its position the
        // device's own encoding, which a failure here says on the console
        // instead of this driver guessing wrong about.
        if (!nvme::is_command_ok(cqe.word3)) {
            char status_text[18];
            auto _ = ort::debug_line(
                {"nvme: the device failed a command, DW3 ", ort::hex(cqe.word3, status_text)});
            return Status::BadState;
        }
        if (data.empty()) {
            complete(index, Status::Ok,
                     std::span{reinterpret_cast<u8 const *>(m_device->data()),
                               usize{parked.count} * prot::block::BLOCK_SIZE});
        } else {
            complete(index, Status::Ok);
        }
        return Status::Ok;
    }

  private:
    nvme::Device *m_device = nullptr;
    u64 m_blocks = 0;
};

class Driver;

/// One Control.Rescan connection: the channel the outgoing directory handed
/// over, parked on the Loop. A readable rescan runs the driver's rescan on
/// the Loop thread -- nothing else runs under it -- and answers the bare
/// status form; a message the decoder refuses closes the connection without
/// an epitaph, as the protocol's contract has it.
class ControlConnection final: public loop::Waiter {
  public:
    ControlConnection() = default;

    auto install(loop::Loop &loop, Driver *driver) -> void {
        m_loop = &loop;
        m_driver = driver;
    }

    /// Whether the connection may take a channel: it holds none.
    [[nodiscard]]
    auto is_free() const -> bool {
        return m_channel == Uandle::Invalid;
    }

    /// Takes |channel| over, the request's, onto the Loop.
    auto connect(Uandle const channel) -> Status {
        auto added = m_loop->add(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                 *static_cast<loop::Waiter *>(this));
        if (added.is_err()) {
            auto _ = ours::handle_close(channel);
            return added.unwrap_err();
        }
        m_channel = channel;
        return Status::Ok;
    }

    /// Defined below Driver, whose rescan() the readable case runs.
    auto on_signals(ours::Signals const observed) -> loop::Action override;

  private:
    /// Frees the connection for the next open and tells the Loop to close
    /// the channel and the slot.
    auto close() -> loop::Action {
        m_channel = Uandle::Invalid;
        return loop::Action::Close;
    }

    Uandle m_channel = Uandle::Invalid;
    loop::Loop *m_loop = nullptr;
    Driver *m_driver = nullptr;
};

constinit std::array<ControlConnection, CONTROL_CONNECTIONS> CONNECTIONS{};

/// The control, the protocol's Handler: an Open of ours.nvme.Control hands
/// its channel to a free connection, or closes it when all four are busy.
class Control final: public ioservice::Handler {
  public:
    Control(loop::Loop &loop, Driver &driver, std::span<ControlConnection> connections)
        : m_connections(connections) {
        for (auto &connection: m_connections) {
            connection.install(loop, &driver);
        }
    }

    auto connect(Uandle const channel) -> void override {
        for (auto &connection: m_connections) {
            if (!connection.is_free()) {
                continue;
            }
            auto const taken = connection.connect(channel);
            if (taken != Status::Ok) {
                auto _ = ort::debug_line(
                    {"nvme: its control refused a connection: ", ours::to_string(taken)});
            }
            return;
        }
        auto _ = ours::handle_close(channel);
    }

  private:
    std::span<ControlConnection> m_connections;
};

/// The storages: the outgoing's, the block server's, the Loop's slots, the
/// window and the claimed function's mapping, BTI, DMA region and device
/// core. They stand before the Driver, whose rescan claims them; the
/// control's connections come after it, because each one calls into it.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS + CONTROL_CONNECTIONS;

constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<NvmeBlockServer::Storage> SERVER_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
constinit ustl::LazyInit<ddk::MappedMmio> WINDOW_STORAGE;
constinit ustl::LazyInit<ddk::MappedMmio> BAR_STORAGE;
constinit ustl::LazyInit<ddk::Bti> BTI_STORAGE;
constinit ustl::LazyInit<ddk::DmaRegion> DMA_STORAGE;
constinit ustl::LazyInit<nvme::Device> DEVICE_STORAGE;

/// The driver: the resources and the window it holds for its life, and the
/// rescan -- the one routine both the boot's first scan and every
/// Control.Rescan run.
class Driver final {
  public:
    Driver(pci::Config config, nvme::Arena arena, Uandle const vma, Uandle const mmio,
           Uandle const iommu)
        : m_config(config), m_arena(arena), m_vma(vma), m_mmio(mmio), m_iommu(iommu) {}

    auto set_server(NvmeBlockServer *server) -> void {
        m_server = server;
    }

    auto set_outgoing(ioservice::Outgoing *outgoing) -> void {
        m_outgoing = outgoing;
    }

    /// The rescan: walk, claim, bring up, publish. NotFound for an
    /// unchanged machine -- the answer, not a failure; otherwise why not,
    /// the claim's own steps' statuses.
    auto rescan() -> Status {
        auto const found =
            nvme::probe(m_config, m_arena, std::span<usize const>{m_owned}.first(m_owned_count));
        if (found.is_err()) {
            return found.unwrap_err();
        }

        // The register file, mapped uncached; the DMA region; the device
        // core over both. The mapped window, the BTI, the region and the
        // Device live for the driver's life in their storages: one claim
        // this round, and a second claim is the next round's work.
        auto bar0 = ddk::MappedMmio::map(m_vma, m_mmio, found->bar0, nvme::REGS_BYTES);
        if (bar0.is_err()) {
            auto _ =
                ort::debug_line({"nvme: cannot map BAR0: ", ours::to_string(bar0.unwrap_err())});
            return bar0.unwrap_err();
        }
        auto &mapped_bar = *BAR_STORAGE.init(std::move(bar0).unwrap());

        auto bti = ddk::Bti::create(m_iommu);
        if (bti.is_err()) {
            auto _ =
                ort::debug_line({"nvme: cannot make its BTI: ", ours::to_string(bti.unwrap_err())});
            return bti.unwrap_err();
        }
        auto &kept_bti = *BTI_STORAGE.init(std::move(bti).unwrap());
        auto dma = ddk::DmaRegion::allocate(m_vma, kept_bti.handle(), nvme::DMA_BYTES);
        if (dma.is_err()) {
            auto _ = ort::debug_line(
                {"nvme: cannot make its DMA region: ", ours::to_string(dma.unwrap_err())});
            return dma.unwrap_err();
        }
        auto &kept_dma = *DMA_STORAGE.init(std::move(dma).unwrap());

        auto &device = *DEVICE_STORAGE.init(mapped_bar.addr(), kept_dma.phys(), kept_dma.virt());

        // Bring-up, in the order the specification fixes. The waits are
        // wall-clock with a per-second keepalive, so the console's silence
        // detector does not NMI the machine mid-bring-up.
        auto const brought = device.bring_up(
            [] { return ours::clock_get_monotonic(); },
            [](u64 const seconds) {
                char text[10];
                auto _ = ort::debug_line({"nvme: waiting for the controller (",
                                          ort::decimal(static_cast<u32>(seconds), text), "s)"});
            });
        if (brought != Status::Ok) {
            auto _ = ort::debug_line({"nvme: bring-up answered ", ours::to_string(brought)});
            return brought;
        }
        auto const blocks = device.identify_blocks();
        if (blocks.is_err()) {
            auto _ =
                ort::debug_line({"nvme: Identify answered ", ours::to_string(blocks.unwrap_err())});
            return blocks.unwrap_err();
        }
        if (blocks.unwrap() == 0) {
            auto _ = ort::debug_line("nvme: the namespace names no blocks");
            return Status::NotFound;
        }
        if (auto const made = device.create_io_queues(); made != Status::Ok) {
            auto _ = ort::debug_line({"nvme: the I/O queues answered ", ours::to_string(made)});
            return made;
        }

        m_server->set_device(device);
        m_server->set_blocks(blocks.unwrap());

        // The block protocol appears with the disk: until a claim, an Open
        // of ours.block.Block is refused by the outgoing directory, which
        // is how a client reads "no device yet" -- an empty geometry would
        // read as a disk of no blocks.
        if (!m_block_served) {
            if (auto const status = m_outgoing->add_protocol(prot::block::BLOCK_NAME, *m_server);
                status != Status::Ok) {
                auto _ =
                    ort::debug_line({"nvme: cannot serve its protocol: ", ours::to_string(status)});
                return status;
            }
            m_block_served = true;
        }

        if (auto const published = publish(); published != Status::Ok) {
            // Logged and not fatal -- the disk still serves its /svc route;
            // the tree merely shows one device fewer.
            auto _ = ort::debug_line({"nvme: its publish answered ", ours::to_string(published)});
        }

        m_owned[m_owned_count] = found->index;
        m_owned_count += 1;
        return Status::Ok;
    }

  private:
    /// The device, published into the registry's tree: a second server end
    /// of the outgoing serves the mount, and the publish names the class,
    /// the proposal and the protocol. One attempt, as before: the mount
    /// handle goes with the message, and a retry would need a fresh pair.
    auto publish() -> Status {
        auto mount_server = Uandle::Invalid;
        auto mount = Uandle::Invalid;
        auto registry_client = Uandle::Invalid;
        auto registry_server = Uandle::Invalid;
        if (auto const made =
                ours::channel_create(ours::ChannelCreateOptions::None, &mount_server, &mount);
            made != Status::Ok) {
            return made;
        }
        if (auto const made = ours::channel_create(ours::ChannelCreateOptions::None,
                                                   &registry_client, &registry_server);
            made != Status::Ok) {
            {
                auto _ = ours::handle_close(mount_server);
            }
            {
                auto _ = ours::handle_close(mount);
            }
            return made;
        }
        if (auto const status = m_outgoing->serve(mount_server); status != Status::Ok) {
            auto _ = ort::debug_line(
                {"nvme: cannot serve its mount channel: ", ours::to_string(status)});
            return status;
        }
        if (auto const status = fsio::connect(prot::dev::REGISTRY_PATH, registry_server);
            status != Status::Ok) {
            auto _ = ort::debug_line(
                {"nvme: cannot reach the device registry: ", ours::to_string(status)});
            return status;
        }
        prot::dev::Client registry{registry_client};
        auto granted = std::array<char, 8>{};
        auto const published = registry.publish("block", "000", prot::block::BLOCK_NAME, mount,
                                                std::span<char>{granted});
        if (published == Status::Ok) {
            auto _ = ort::debug_line(
                {"nvme: its device is in the tree as block/", std::string_view{granted.data()}});
        }
        return published;
    }

    pci::Config m_config;
    nvme::Arena m_arena;
    Uandle m_vma;
    Uandle m_mmio;
    Uandle m_iommu;
    NvmeBlockServer *m_server = nullptr;
    ioservice::Outgoing *m_outgoing = nullptr;
    /// The functions this driver is running, which a rescan skips.
    std::array<usize, 4> m_owned{};
    usize m_owned_count = 0;
    /// Whether the block protocol is in the outgoing already: the first
    /// claim adds it, and the rescan of a second claim must not.
    bool m_block_served = false;
};

auto ControlConnection::on_signals(ours::Signals const observed) -> loop::Action {
    if (!(observed & ours::Signals::Readable)) {
        // The peer went with nothing to read: the slot's close is the
        // reply there is to give.
        return close();
    }
    auto bytes = std::array<u8, wire::HEADER_SIZE>{};
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    auto const read = ours::channel_read(
        m_channel, ours::ChannelReadOptions::None, bytes.data(), handles.data(),
        static_cast<u32>(bytes.size()), static_cast<u32>(handles.size()), &num_bytes, &num_handles);
    for (auto index = u32{0}; index < num_handles; ++index) {
        auto _ = ours::handle_close(handles[index]);
    }
    if (read != Status::Ok) {
        return close();
    }
    auto const decoded = prot::nvme::decode_rescan(
        wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}});
    if (decoded.is_err()) {
        // Refused, no epitaph: the connection closes.
        return close();
    }
    auto const answer = m_driver->rescan();
    auto reply_bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    auto const reply =
        prot::nvme::encode_rescan_reply(reply_bytes, decoded->txid, {.status = answer});
    if (reply.is_err()) {
        return close();
    }
    if (auto const written =
            ours::channel_write(m_channel, ours::ChannelWriteOptions::None, reply->bytes.data(),
                                static_cast<u32>(reply->bytes.size()), nullptr, 0);
        written != Status::Ok) {
        return close();
    }
    return loop::Action::Rearm;
}

} // namespace

auto main(int, char **) -> int {
    // Every root this driver asks for: Mmio for the window and the BAR,
    // Iommu for the DMA pin. A polling driver holds no interrupt line and
    // no ports.
    ddk::Resources resources;

    auto const vma = ddk::startup_handle(ours::HandleType::VmaRoot, "VmaRoot");
    if (vma.is_err()) {
        return 1;
    }
    auto const directory_request =
        ddk::startup_handle(ours::HandleType::DirectoryRequest, "a DirectoryRequest handle");
    if (directory_request.is_err()) {
        return 1;
    }

    auto const mmio_resource = resources.take(ddk::Kind::Mmio);
    if (mmio_resource.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: no MmIo resource: ", ours::to_string(mmio_resource.unwrap_err())});
        return 1;
    }
    auto const iommu_resource = resources.take(ddk::Kind::Iommu);
    if (iommu_resource.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: no Iommu resource: ", ours::to_string(iommu_resource.unwrap_err())});
        return 1;
    }

    // Where the ECAM window is: the route service reads it from the MCFG
    // table, as virtio-blk's driver does for its enumeration. Behind it is
    // where a rescan assigns new BARs from, the decode space the firmware
    // left unassigned.
    auto const ecam = prot::acpi::ecam_base(0);
    if (ecam.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: no ECAM base from the route service: ", ours::to_string(ecam.unwrap_err())});
        return 1;
    }
    auto ecam_window = ddk::MappedMmio::map(vma.unwrap(), mmio_resource.unwrap(),
                                            ours::PhysAddr{ecam->address}, usize{ecam->size});
    if (ecam_window.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: cannot map the ECAM window: ", ours::to_string(ecam_window.unwrap_err())});
        return 1;
    }
    auto &mapped_window = *WINDOW_STORAGE.init(std::move(ecam_window).unwrap());
    auto window_bytes = std::span<u8>{
        reinterpret_cast<u8 *>(std::to_underlying(mapped_window.addr())), usize{ecam->size}};
    auto config = pci::Config{window_bytes};
    auto arena = nvme::Arena{ours::PhysAddr{ecam->address + ecam->size},
                             ours::PhysAddr{ecam->address + ecam->size + ARENA_BYTES}};

    // The machine: the port, the Loop, the outgoing, the block server, the
    // control. All of it stands up before the first rescan, because the
    // driver is resident -- the routes exist whether or not a device does.
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"nvme: cannot make its port: ", ours::to_string(status)});
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    auto &outgoing_storage = *OUTGOING_STORAGE.init();
    ioservice::Outgoing outgoing(outgoing_storage, loop);
    auto &storage = *SERVER_STORAGE.init();
    NvmeBlockServer server(storage, loop);

    Driver driver(config, arena, vma.unwrap(), mmio_resource.unwrap(), iommu_resource.unwrap());
    driver.set_server(&server);
    driver.set_outgoing(&outgoing);

    Control control(loop, driver, std::span<ControlConnection>{CONNECTIONS});
    if (auto const status = outgoing.add_protocol(prot::nvme::CONTROL_NAME, control);
        status != Status::Ok) {
        auto _ = ort::debug_line({"nvme: cannot serve its control: ", ours::to_string(status)});
        return 1;
    }
    // ours.block.Block is added by the first claim, not here: until a
    // controller is running, an Open of it is refused, which is how a
    // client reads "no device yet".
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        auto _ = ort::debug_line(
            {"nvme: cannot serve its outgoing directory: ", ours::to_string(status)});
        return 1;
    }

    // The boot's own first rescan. NotFound leaves the driver resident: it
    // answers its control until a device appears.
    auto const first = driver.rescan();
    if (first != Status::Ok) {
        auto _ = ort::debug_line({"nvme: no NVMe controller on bus 0 (", ours::to_string(first),
                                  "); resident, waiting for a rescan"});
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line(
        {"nvme: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
