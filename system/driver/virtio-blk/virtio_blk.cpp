/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// virtio-blk -- the userspace driver of QEMU's virtio block device
/// (storage-layer plan, .agents/tasks/storage-layer; the PCI hot-plug round
/// made it resident), as a component that provides ours.block.Block: a
/// client opens the protocol, asks for the device's geometry or for blocks
/// by number, and the driver drives the hardware on its behalf. Root.cm
/// offers it under the name ours.block.Nvme's sibling ours.block.Virtio,
/// and its control under ours.virtio.Control.
///
/// Resident, not scan-once: a boot without the device finds no
/// vendor-0x1AF4 block function and stays up anyway, serving
/// ours.virtio.Control. A rescan -- the boot's own first one, or a
/// Control.Rescan a program asked for -- walks the ECAM window through
/// system/lib/pci, claims the first function of the family that is not
/// already running, sizes and assigns its I/O BAR0 if it arrived with no
/// base (hot-plug: firmware assigns BARs at boot only), takes its I/O
/// ports and its interrupt line, brings the device up through
/// system/lib/virtio's device core, and publishes the disk into the
/// registry's tree.
///
/// The device is driven through the legacy (transitional) interface: the
/// I/O-port BAR that QEMU's virtio-pci keeps beside the modern one, whose
/// registers are one field each, at one offset, with one width. The legacy
/// interface is the working path, and why it is tells you what the modern
/// one costs here: the modern common config's queue section does not answer
/// on this QEMU build -- queue_size reads 0xFFFF, num_queues 0, and a queue
/// whose size the device never states is never configured, so its doorbell
/// is ignored (evidence and the monitor ground truth:
/// .agents/tasks/storage-layer/QEMU-CFG-FINDINGS.md). The legacy registers
/// answered from the first read.
///
/// The objects:
///   - the MmIo root resource, through /svc: the ECAM window the
///     enumeration reads comes from vmo_create_physical; it is kept for the
///     driver's life, because every rescan walks the window again;
///   - the Ioport root resource, likewise kept: each claim's
///     ioports_request opens the function's I/O range to this process;
///   - the IOMMU System resource, likewise kept: each claim makes its own
///     BTI and its own pinned region for the ring;
///   - the Irq root resource, likewise kept: each claim takes its line,
///     whose GSI the chipset's wiring computes from the slot the probe
///     named;
///   - the Loop, on which the outgoing directory, every client connection
///     and the control's connections wait;
///   - the server, block_server::BlockServer over the claimed device: the
///     library owns the connections, the decode and the reply, and the
///     driver's start() runs one request to completion -- the poll is on
///     the same thread as everything else, so requests serialize, which
///     one device queue makes true anyway.
///
/// The events: the root routes a client's Open into the outgoing directory
/// (design D11); the server takes the channel, and each Read on it parks in
/// the slot, writes the request header, offers the descriptor chain through
/// the available ring, rings the queue, and parks on the interrupt line
/// until the device answers, whose answer complete() turns into the reply.
/// A rescan's bring-up runs inside the control's on_signals(), on the Loop
/// thread: nothing else runs under it, which is what makes the
/// one-at-a-time device safe to re-enter. Before the driver leaves, the
/// device is stopped and observed stopped, so the kernel never has to
/// quarantine what it pinned.
#include <ours/bti.hpp>
#include <ours/interrupt.hpp>
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <cstdint>

#include <block_server/block_server.hpp>
#include <ddk/ddk.hpp>
#include <fsio/fsio.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <prot/acpi_client.hpp>
#include <prot/block.hpp>
#include <prot/dev.hpp>
#include <prot/dev_client.hpp>
#include <prot/resource.hpp>
#include <prot/resource_client.hpp>
#include <prot/virtio_control.hpp>
#include <server/server.hpp>
#include <ustl/lazy_init.hpp>
#include <virtio/virtio.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Signals;
using ours::Status;
using ours::Uandle;

/// How many Control.Rescan connections may be open at once: the shell's
/// rescan is one request on one connection of its own, and a second open
/// before the first's reply lands is refused with its channel closed.
constexpr auto CONTROL_CONNECTIONS = usize{4};

/// The I/O-port range a rescan assigns new BARs from: 4 KiB of the 16-bit
/// port space's tail, far more than the disks this machine will hot-add.
constexpr u16 IO_ARENA_BASE = 0xE000;
constexpr u16 IO_ARENA_LIMIT = 0xF000;

/// How many functions this driver may run at once: one disk this round,
/// and a second claim is the next round's work.
constexpr auto MAX_OWNED = usize{4};

/// The port I/O the legacy registers are reached through: the x86 in and
/// out, which ioports_request has opened to this process's threads for the
/// function's range. An I/O port is not memory, and reading it as one is a
/// page fault on the port address.
class IoPorts final: public virtio::Ports {
  public:
    explicit IoPorts(u16 const base) : m_base(base) {}

    auto in8(u16 const offset) const -> u8 override {
        auto value = u8{0};
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(m_base + offset)));
        return value;
    }
    auto in16(u16 const offset) const -> u16 override {
        auto value = u16{0};
        asm volatile("inw %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(m_base + offset)));
        return value;
    }
    auto in32(u16 const offset) const -> u32 override {
        auto value = u32{0};
        asm volatile("inl %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(m_base + offset)));
        return value;
    }
    auto out8(u16 const offset, u8 const value) const -> void override {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(static_cast<u16>(m_base + offset)));
    }
    auto out16(u16 const offset, u16 const value) const -> void override {
        asm volatile("outw %0, %1" : : "a"(value), "Nd"(static_cast<u16>(m_base + offset)));
    }
    auto out32(u16 const offset, u32 const value) const -> void override {
        asm volatile("outl %0, %1" : : "a"(value), "Nd"(static_cast<u16>(m_base + offset)));
    }

  private:
    u16 m_base;
};

/// The block service's shape: two clients at once, and the device works on
/// one request at a time -- the poll model, MAX_INFLIGHT the tag depth the
/// one request region answers.
constexpr auto MAX_CLIENTS = usize{2};
constexpr auto MAX_INFLIGHT = usize{1};

/// The server: the block-driver library's BlockServer over the claimed
/// device. The library owns the connections, the decode, the slot table
/// and the reply; this class is the hardware, the two hooks the library
/// calls. Until a rescan claims a device there is none, and start()
/// answers NotFound: the protocol is served, the disk is not there yet.
class VirtioBlockServer final
    : public block_server::BlockServer<VirtioBlockServer, MAX_CLIENTS, MAX_INFLIGHT> {
  public:
    using Base = block_server::BlockServer<VirtioBlockServer, MAX_CLIENTS, MAX_INFLIGHT>;
    using Storage = typename Base::Storage;

    VirtioBlockServer(Storage &storage, loop::Loop &loop) : Base(storage, loop) {}

    auto set_device(virtio::Device *device) -> void {
        m_device = device;
    }

    /// GetInfo's answer: the legacy device's block is a 512-byte sector,
    /// and the geometry is what setup read from the device.
    [[nodiscard]]
    auto geometry() const -> block_server::Geometry {
        return {.block_size = prot::block::BLOCK_SIZE, .blocks = m_blocks};
    }

    auto set_blocks(u64 const blocks) -> void {
        m_blocks = blocks;
    }

    /// The library's hand-off: run the parked request against the device,
    /// and complete it before returning. |data| is a Write's blocks,
    /// placed where the device reads them from -- the one copy of a Write.
    /// The park is the interrupt wait: the thread parks on the device's
    /// line until it answers, as the hardware path is.
    auto start(usize const index, std::span<u8 const> const data) -> Status {
        if (m_device == nullptr || m_interrupt == Uandle::Invalid) {
            return Status::NotFound;
        }
        auto const &parked = slot(index);
        auto const park = [this] { return ours::interrupt_wait(m_interrupt, nullptr); };
        if (data.empty()) {
            // A Read: the device writes the blocks into the region's data
            // area, where the completion's encoder reads them from -- no
            // copy but the one into the reply.
            auto const status = m_device->read_blocks(parked.lba, parked.count, park);
            if (status != Status::Ok) {
                return status;
            }
            complete(
                index, Status::Ok,
                std::span{reinterpret_cast<u8 const *>(std::to_underlying(m_region_map) +
                                                       virtio::REQUEST_OFFSET + virtio::HEADER),
                          usize{parked.count} * virtio::SECTOR});
            return Status::Ok;
        }
        std::memcpy(reinterpret_cast<void *>(std::to_underlying(m_region_map) +
                                             virtio::REQUEST_OFFSET + virtio::HEADER),
                    data.data(), data.size());
        auto const status = m_device->write_blocks(parked.lba, parked.count, park);
        if (status != Status::Ok) {
            return status;
        }
        complete(index, Status::Ok);
        return Status::Ok;
    }

    /// The interrupt line the park waits on, the claim's to hand over.
    auto set_interrupt(Uandle const interrupt) -> void {
        m_interrupt = interrupt;
    }

    auto set_region(ours::VirtAddr const region_map) -> void {
        m_region_map = region_map;
    }

  private:
    virtio::Device *m_device = nullptr;
    Uandle m_interrupt = Uandle::Invalid;
    /// The region's CPU mapping, where the device leaves the data a Read
    /// answers with.
    ours::VirtAddr m_region_map = ours::VirtAddr{0};
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

/// The control, the protocol's Handler: an Open of ours.virtio.Control
/// hands its channel to a free connection, or closes it when all four are
/// busy.
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
                    {"virtio-blk: its control refused a connection: ", ours::to_string(taken)});
            }
            return;
        }
        auto _ = ours::handle_close(channel);
    }

  private:
    std::span<ControlConnection> m_connections;
};

constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS + CONTROL_CONNECTIONS;
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);
static_assert(virtio::DATA_OFFSET + prot::block::MAX_READ_BLOCKS * virtio::SECTOR + 1 <=
              virtio::REGION_BYTES);

/// The storages: the outgoing's, the block server's, the Loop's slots, the
/// ECAM window, and each claim's ports, line, BTI, region and device. They
/// stand before the Driver, whose rescan claims them; the control's
/// connections come after it, because each one calls into it.
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<VirtioBlockServer::Storage> SERVER_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
constinit ustl::LazyInit<ddk::MappedMmio> WINDOW_STORAGE;
constinit ustl::LazyInit<IoPorts> PORTS_STORAGE;
constinit ustl::LazyInit<Uandle> IRQ_STORAGE;
constinit ustl::LazyInit<ddk::Bti> BTI_STORAGE;
constinit ustl::LazyInit<ddk::DmaRegion> REGION_STORAGE;
constinit ustl::LazyInit<virtio::Device> DEVICE_STORAGE;

/// The driver: the resources and the window it holds for its life, and the
/// rescan -- the one routine both the boot's first scan and every
/// Control.Rescan run.
class Driver final {
  public:
    Driver(pci::Config config, Uandle const vma, Uandle const ioport, Uandle const iommu,
           Uandle const irq)
        : m_config(config), m_vma(vma), m_ioport(ioport), m_iommu(iommu), m_irq(irq) {}

    auto set_server(VirtioBlockServer *server) -> void {
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
            virtio::probe(m_config, m_arena, std::span<usize const>{m_owned}.first(m_owned_count));
        if (found.is_err()) {
            return found.unwrap_err();
        }

        // The function's I/O ports, opened to this process's threads; the
        // interrupt line; the DMA ring; the device core over all of it.
        // Each claim's objects live for the driver's life in their
        // storages: one claim this round, and a second claim is the next
        // round's work.
        if (auto const status = ours::ioports_request(m_ioport, found->io_base, 0x80);
            status != Status::Ok) {
            auto _ = ort::debug_line(
                {"virtio-blk: cannot take its I/O ports: ", ours::to_string(status)});
            return status;
        }

        // The interrupt line, on the GSI the chipset's wiring names for the
        // slot the probe found: a function at root-bus slot S, pin A, is
        // wired to GSI 20 + ((S + 0) & 3) on this machine -- the boot disk
        // sat at slot 3, whose GSI 23 the first driver carried as a
        // constant. A PciIrq method on the route service, planned with the
        // driver manager, will name the line from the firmware's tables
        // instead of this chipset rule. PCI INTx is level and active low,
        // which ModeLevelLow says.
        auto const slot = static_cast<u32>(found->index / pci::FUNCTIONS_PER_DEVICE);
        auto const gsi = static_cast<u32>(20 + (slot & 3));
        auto interrupt = ddk::irq::take(m_irq, ours::ROOT_INTERRUPT_DOMAIN, gsi,
                                        ours::InterruptOptions::ModeLevelLow);
        if (interrupt.is_err()) {
            auto _ = ort::debug_line({"virtio-blk: cannot take its interrupt line: ",
                                      ours::to_string(interrupt.unwrap_err())});
            return interrupt.unwrap_err();
        }
        auto &line = *IRQ_STORAGE.init(interrupt.unwrap());

        auto bti = ddk::Bti::create(m_iommu);
        if (bti.is_err()) {
            auto _ = ort::debug_line(
                {"virtio-blk: cannot make its BTI: ", ours::to_string(bti.unwrap_err())});
            return bti.unwrap_err();
        }
        auto &kept_bti = *BTI_STORAGE.init(std::move(bti).unwrap());
        auto region = ddk::DmaRegion::allocate(m_vma, kept_bti.handle(), virtio::REGION_BYTES);
        if (region.is_err()) {
            auto _ = ort::debug_line(
                {"virtio-blk: cannot make its DMA region: ", ours::to_string(region.unwrap_err())});
            return region.unwrap_err();
        }
        auto &kept_region = *REGION_STORAGE.init(std::move(region).unwrap());

        auto &ports = *PORTS_STORAGE.init(found->io_base);
        auto &device = *DEVICE_STORAGE.init(ports, kept_region.virt(), kept_region.phys());

        // Bring-up, in the order the legacy specification fixes.
        if (auto const status = device.bring_up(kept_region.phys()); status != Status::Ok) {
            auto _ = ort::debug_line({"virtio-blk: bring-up answered ", ours::to_string(status)});
            return status;
        }
        auto const blocks = device.blocks();
        if (blocks == 0) {
            auto _ = ort::debug_line("virtio-blk: the device names no blocks");
            return Status::NotFound;
        }

        m_server->set_device(&device);
        m_server->set_blocks(blocks);
        m_server->set_interrupt(line);
        m_server->set_region(kept_region.virt());

        // The block protocol appears with the disk: until a claim, an Open
        // of ours.block.Block is refused by the outgoing directory, which
        // is how a client reads "no device yet" -- an empty geometry would
        // read as a disk of no blocks.
        if (!m_block_served) {
            if (auto const status = m_outgoing->add_protocol(prot::block::BLOCK_NAME, *m_server);
                status != Status::Ok) {
                auto _ = ort::debug_line(
                    {"virtio-blk: cannot serve its protocol: ", ours::to_string(status)});
                return status;
            }
            m_block_served = true;
        }

        if (auto const published = publish(); published != Status::Ok) {
            // Logged and not fatal -- the disk still serves its /svc route;
            // the tree merely shows one device fewer.
            auto _ =
                ort::debug_line({"virtio-blk: its publish answered ", ours::to_string(published)});
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
                {"virtio-blk: cannot serve its mount channel: ", ours::to_string(status)});
            return status;
        }
        if (auto const status = fsio::connect(prot::dev::REGISTRY_PATH, registry_server);
            status != Status::Ok) {
            auto _ = ort::debug_line(
                {"virtio-blk: cannot reach the device registry: ", ours::to_string(status)});
            return status;
        }
        prot::dev::Client registry{registry_client};
        auto granted = std::array<char, 8>{};
        auto const published = registry.publish("block", "000", prot::block::BLOCK_NAME, mount,
                                                std::span<char>{granted});
        if (published == Status::Ok) {
            auto _ = ort::debug_line({"virtio-blk: its device is in the tree as block/",
                                      std::string_view{granted.data()}});
        }
        return published;
    }

    pci::Config m_config;
    virtio::IoArena m_arena{IO_ARENA_BASE, IO_ARENA_LIMIT};
    Uandle m_vma;
    Uandle m_ioport;
    Uandle m_iommu;
    Uandle m_irq;
    VirtioBlockServer *m_server = nullptr;
    ioservice::Outgoing *m_outgoing = nullptr;
    /// The functions this driver is running, which a rescan skips.
    std::array<usize, MAX_OWNED> m_owned{};
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
    auto const decoded = prot::virtio::decode_rescan(
        wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}});
    if (decoded.is_err()) {
        // Refused, no epitaph: the connection closes.
        return close();
    }
    auto const answer = m_driver->rescan();
    auto reply_bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    auto const reply =
        prot::virtio::encode_rescan_reply(reply_bytes, decoded->txid, {.status = answer});
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

constinit std::array<ControlConnection, CONTROL_CONNECTIONS> CONNECTIONS{};

} // namespace

auto main(int, char **) -> int {
    // The route service's client connects through fsio, so the namespace is
    // installed first.
    if (auto const installed = fsio::install(); installed != Status::Ok) {
        auto _ = ort::debug_line(
            {"virtio-blk: cannot install its namespace: ", ours::to_string(installed)});
        return 1;
    }

    // Every root this driver asks for, with the ladder's destructor closing
    // whatever is still held on any early return below. Each root is kept
    // for the driver's life: every rescan that claims a function walks the
    // window, opens ports, makes a BTI and takes a line.
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
            {"virtio-blk: no MmIo resource: ", ours::to_string(mmio_resource.unwrap_err())});
        return 1;
    }
    auto const ioport_resource = resources.take(ddk::Kind::Ioport);
    if (ioport_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Ioport resource: ", ours::to_string(ioport_resource.unwrap_err())});
        return 1;
    }
    auto const iommu_resource = resources.take(ddk::Kind::Iommu);
    if (iommu_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Iommu resource: ", ours::to_string(iommu_resource.unwrap_err())});
        return 1;
    }
    auto const irq_resource = resources.take(ddk::Kind::Irq);
    if (irq_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Irq resource: ", ours::to_string(irq_resource.unwrap_err())});
        return 1;
    }

    // Bus 0 of the ECAM, one window for the whole enumeration, kept for the
    // driver's life: every rescan walks it again.
    auto const ecam = prot::acpi::ecam_base(0);
    if (ecam.is_err()) {
        auto _ = ort::debug_line({"virtio-blk: no ECAM base from the route service: ",
                                  ours::to_string(ecam.unwrap_err())});
        return 1;
    }
    auto ecam_window = ddk::MappedMmio::map(vma.unwrap(), mmio_resource.unwrap(),
                                            ours::PhysAddr{ecam->address}, usize{ecam->size});
    if (ecam_window.is_err()) {
        auto _ = ort::debug_line({"virtio-blk: cannot map the ECAM window: ",
                                  ours::to_string(ecam_window.unwrap_err())});
        return 1;
    }
    auto &mapped_window = *WINDOW_STORAGE.init(std::move(ecam_window).unwrap());
    auto window_bytes = std::span<u8>{
        reinterpret_cast<u8 *>(std::to_underlying(mapped_window.addr())), usize{ecam->size}};
    auto config = pci::Config{window_bytes};

    // The machine: the port, the Loop, the outgoing, the block server, the
    // control. All of it stands up before the first rescan, because the
    // driver is resident -- the routes exist whether or not a device does.
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"virtio-blk: cannot make its port: ", ours::to_string(status)});
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    auto &outgoing_storage = *OUTGOING_STORAGE.init();
    ioservice::Outgoing outgoing(outgoing_storage, loop);
    auto &storage = *SERVER_STORAGE.init();
    VirtioBlockServer server(storage, loop);

    Driver driver(config, vma.unwrap(), ioport_resource.unwrap(), iommu_resource.unwrap(),
                  irq_resource.unwrap());
    driver.set_server(&server);
    driver.set_outgoing(&outgoing);

    Control control(loop, driver, std::span<ControlConnection>{CONNECTIONS});
    if (auto const status = outgoing.add_protocol(prot::virtio::CONTROL_NAME, control);
        status != Status::Ok) {
        auto _ =
            ort::debug_line({"virtio-blk: cannot serve its control: ", ours::to_string(status)});
        return 1;
    }
    // ours.block.Block is added by the first claim, not here: until a
    // device is running, an Open of it is refused, which is how a client
    // reads "no device yet".
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        auto _ = ort::debug_line(
            {"virtio-blk: cannot serve its outgoing directory: ", ours::to_string(status)});
        return 1;
    }

    // The boot's own first rescan. NotFound leaves the driver resident: it
    // answers its control until a device appears.
    auto const first = driver.rescan();
    if (first != Status::Ok) {
        auto _ = ort::debug_line({"virtio-blk: no virtio block device on bus 0 (",
                                  ours::to_string(first), "); resident, waiting for a rescan"});
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line(
        {"virtio-blk: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
