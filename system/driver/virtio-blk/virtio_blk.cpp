/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// virtio-blk -- the userspace driver of QEMU's virtio block device
/// (storage-layer plan, .agents/tasks/storage-layer), as a component that
/// provides ours.block.Block: a client opens the protocol, asks for the
/// device's geometry or for blocks by number, and the driver drives the
/// hardware on its behalf.
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
///     enumeration reads and the BARs it reports come from
///     vmo_create_physical;
///   - the Ioport root resource: ioports_request opens the device's I/O
///     ports to this process's threads, and the legacy registers are read
///     and written with in and out from then on;
///   - the IOMMU System resource: a stub IOMMU, one BTI for the device, and
///     the pin that hands the ring and the request buffer to the device by
///     one physical address;
///   - the ring: one contiguous run of pages from vmo_create_contiguous.
///     The legacy layout puts the used ring on the page boundary after the
///     available ring, and the request's header, data and status live
///     behind it;
///   - the Loop, on which the outgoing directory and every client
///     connection wait;
///   - the server, block_server::BlockServer over this device: the
///     library owns the connections, the decode and the reply, and the
///     driver's start() runs one request to completion -- the poll is on
///     the same thread as everything else, so requests serialize, which
///     one device queue makes true anyway. The library's async path, an
///     interrupt-driven completion, waits on the kernel to deliver an
///     interrupt into a Loop (docs/repairs/261103/kernel-interrupts.md);
///     this driver's single-request region keeps it synchronous either way.
///
/// The events: the root routes a client's Open into the outgoing directory
/// (design D11); the server takes the channel, and each Read on it parks in
/// the slot, writes the request header, offers the descriptor chain through
/// the available ring, rings the queue, and polls the used ring until the
/// device answers, whose answer complete() turns into the reply. Before the
/// driver leaves, the device is stopped and observed stopped, so the kernel
/// never has to quarantine what it pinned.
#include <ours/bti.hpp>
#include <ours/interrupt.hpp>
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <string_view>

#include <cstdint>

#include <block_server/block_server.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <ours_block/ours_block.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>
#include <server/server.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Signals;
using ours::Status;
using ours::Uandle;

/// The ECAM base on q35 with OVMF (SeaBIOS would be 0xB0000000). Userspace
/// cannot read ACPI's MCFG table, so the firmware's choice is a fact of the
/// image, spelled here and wrong nowhere else.
constexpr auto ECAM_BASE = ours::PhysAddr{0xE000'0000};

/// One bus of configuration space, and the registers the enumeration reads,
/// in bytes from a function's base.
constexpr auto BUS_BYTES = usize{1 << 20};
constexpr auto CFG_VENDOR_ID = usize{0x00};
constexpr auto CFG_DEVICE_ID = usize{0x02};
constexpr auto CFG_BAR_0 = usize{0x10};

/// The virtio vendor, and the block device's id in both forms.
constexpr auto VIRTIO_VENDOR = u16{0x1AF4};
constexpr auto VIRTIO_BLK_TRANSITIONAL = u16{0x1001};
constexpr auto VIRTIO_BLK_MODERN = u16{0x1042};

/// The device status bits, in the order the specification demands them.
constexpr auto STATUS_ACKNOWLEDGE = u8{1};
constexpr auto STATUS_DRIVER = u8{2};
constexpr auto STATUS_DRIVER_OK = u8{4};
constexpr auto STATUS_FEATURES_OK = u8{8};

/// A descriptor's flags.
constexpr auto DESC_NEXT = u16{1};
constexpr auto DESC_WRITE = u16{2};

/// One page, the unit a legacy ring's page frame number names.
constexpr auto PAGE = usize{4096};

/// One sector, plus the request header that precedes the data and the
/// status byte that follows it.
constexpr auto SECTOR = usize{512};
constexpr auto HEADER = usize{16};

/// The GSI the device's INTx line is wired to on this machine's chipset:
/// the first PCI function QEMU adds, slot 3 pin A.
constexpr auto GSI_DISK = u32{23};

/// The queue's depth, the driver's choice within the device's maximum. A
/// legacy ring is the descriptor table, the available ring after it, and
/// the used ring on the next page boundary, so it spans two pages whatever
/// the depth: the used ring always starts on the page after the one the
/// page frame number names.
constexpr auto LEGACY_QUEUE_NUM = usize{256};
constexpr auto DESC_BYTES = LEGACY_QUEUE_NUM * 16;
constexpr auto AVAIL_OFFSET = DESC_BYTES;
constexpr auto AVAIL_BYTES = 6 + LEGACY_QUEUE_NUM * 2;
constexpr auto USED_OFFSET = (AVAIL_OFFSET + AVAIL_BYTES + PAGE - 1) & ~(PAGE - 1);
constexpr auto USED_END = USED_OFFSET + 6 + LEGACY_QUEUE_NUM * 8;

/// The contiguous run: the ring's pages, then the request behind the used
/// ring, on a 16-byte boundary. Its tail holds the biggest request the
/// protocol carries: header, MAX_READ_SECTORS blocks, status.
constexpr auto REGION_PAGES = usize{4};
constexpr auto REGION_BYTES = REGION_PAGES * PAGE;
constexpr auto REQUEST_OFFSET = (USED_END + 15) & ~usize{15};
constexpr auto DATA_OFFSET = REQUEST_OFFSET + HEADER;

/// How many clients may be connected at once. One is all dev's routing
/// checks use; a client past them is refused with NoResource.
constexpr auto MAX_CLIENTS = usize{2};

/// How many requests the hardware works on at once: one, the region's
/// single request layout being why. See VirtioBlockServer.
constexpr auto MAX_INFLIGHT = usize{1};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);
static_assert(DATA_OFFSET + ours_block::MAX_READ_BLOCKS * SECTOR + 1 <= REGION_BYTES);

/// Configuration space as this driver sees it: a mapped window plus the
/// offset of one function in it.
class Config final {
  public:
    Config(ours::VirtAddr base, usize function_offset) : m_base(base), m_offset(function_offset) {}

    [[nodiscard]]
    auto read16(usize const offset) const -> u16 {
        return *reinterpret_cast<volatile u16 const *>(m_base + m_offset + offset);
    }

    [[nodiscard]]
    auto read32(usize const offset) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_base + m_offset + offset);
    }

  private:
    ours::VirtAddr m_base;
    usize m_offset;
};

auto to_function_offset(u8 const device) -> usize {
    return usize{device} << 15;
}

/// Maps [paddr, paddr + size) into this process with the MmIo root, through a
/// physical VMO and the startup VMA. Uncached, as every mapping of a physical
/// VMO is.
auto map_mmio(Uandle const vma, Uandle const mmio_resource, ours::PhysAddr const paddr,
              usize const size, ours::VirtAddr *out) -> Status {
    auto window = Uandle::Invalid;
    if (auto const status = ours::vmo_create_physical(mmio_resource, paddr, size, &window);
        status != Status::Ok) {
        return status;
    }
    auto const mapped = ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite,
                                      0, window, 0, size, out);
    // The mapping holds no reference to the VMO once made, so the handle goes
    // either way: the window's lifetime is the mapping's now.
    auto _ = ours::handle_close(window);
    return mapped;
}

/// The legacy registers, at |base| in this process's I/O space. One field
/// each, at one offset, with one width -- the shape the modern common config
/// lacks on this build.
class LegacyRegs final {
  public:
    explicit LegacyRegs(u16 const base) : m_base(base) {}

    /// HOST_FEATURES, and the features the driver accepts: a legacy driver
    /// needs none of the device's.
    [[nodiscard]]
    auto host_features() const -> u32 {
        return in32(0x00);
    }

    auto accept_no_features() const -> void {
        out32(0x04, 0);
    }

    /// QUEUE_PFN: the ring's physical address, named by page frame.
    auto set_queue_pfn(ours::PhysAddr const paddr) const -> void {
        out32(0x08, static_cast<u32>(paddr >> 12));
    }

    /// QUEUE_NUM: the depth the device runs this queue at, read as its
    /// maximum and written as the driver's choice.
    [[nodiscard]]
    auto queue_num() const -> u16 {
        return in16(0x0C);
    }

    auto set_queue_num(u16 const num) const -> void {
        out16(0x0C, num);
    }

    auto select_queue(u16 const index) const -> void {
        out16(0x0E, index);
    }

    /// QUEUE_NOTIFY: rung with the queue's index.
    auto notify(u16 const index) const -> void {
        out16(0x10, index);
    }

    [[nodiscard]]
    auto status() const -> u8 {
        return in8(0x12);
    }

    auto set_status(u8 const value) const -> void {
        out8(0x12, value);
    }

    /// ISR status: which of the ring and the config asked for attention. One
    /// read answers and clears it, which drops the device's interrupt line --
    /// what the driver does after each wait, so the level the device holds
    /// does not re-fire the moment the kernel unmasks.
    [[nodiscard]]
    auto isr() const -> u8 {
        return in8(0x13);
    }

    /// The device config's capacity, in blocks: two reads of the u64 the
    /// legacy layout puts behind the common registers, without MSI-X enabled.
    [[nodiscard]]
    auto capacity() const -> u64 {
        return static_cast<u64>(in32(0x14)) | (static_cast<u64>(in32(0x18)) << 32);
    }

  private:
    // The x86 I/O instructions, which ioports_request has opened to this
    // process's threads: an I/O port is not memory, and reading it as one is
    // a page fault on the port address.
    [[nodiscard]]
    auto in8(u16 const offset) const -> u8 {
        auto value = u8{0};
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(m_base + offset)));
        return value;
    }

    [[nodiscard]]
    auto in16(u16 const offset) const -> u16 {
        auto value = u16{0};
        asm volatile("inw %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(m_base + offset)));
        return value;
    }

    [[nodiscard]]
    auto in32(u16 const offset) const -> u32 {
        auto value = u32{0};
        asm volatile("inl %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(m_base + offset)));
        return value;
    }

    auto out8(u16 const offset, u8 const value) const -> void {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(static_cast<u16>(m_base + offset)));
    }

    auto out16(u16 const offset, u16 const value) const -> void {
        asm volatile("outw %0, %1" : : "a"(value), "Nd"(static_cast<u16>(m_base + offset)));
    }

    auto out32(u16 const offset, u32 const value) const -> void {
        asm volatile("outl %0, %1" : : "a"(value), "Nd"(static_cast<u16>(m_base + offset)));
    }

    u16 m_base;
};

/// The device, once brought up: everything the ring and the requests touch.
/// One instance, in Storage below, and only the Loop's thread reaches it.
class Device final {
  public:
    auto init(u16 const io_ports, ours::VirtAddr const region_map_address,
              ours::PhysAddr const region_base, ours::Uandle const interrupt) -> void {
        m_regs = LegacyRegs{io_ports};
        m_region_map = region_map_address;
        m_region_phys = region_base;
        m_interrupt = interrupt;
    }

    /// The device's capacity, in blocks, for GetInfo.
    [[nodiscard]]
    auto blocks() const -> u64 {
        return m_regs.capacity();
    }

    /// Reads |count| blocks starting at |lba| into the region's data area,
    /// and returns the region offset the data landed at -- the caller reads
    /// it there, through the same mapping. The whole request runs on the
    /// caller's thread: the header and descriptors are written, the chain
    /// offered, the queue rung, and the thread parks in interrupt_wait()
    /// until the device answers. Requests serialize on this, which one device
    /// queue makes true anyway.
    [[nodiscard]]
    auto read_blocks(u64 const lba, usize const count) -> Status {
        return submit(lba, count, false);
    }

    /// Writes |count| blocks starting at |lba| from the region's data area,
    /// which the caller filled before the call.
    [[nodiscard]]
    auto write_blocks(u64 const lba, usize const count) -> Status {
        return submit(lba, count, true);
    }

  private:
    /// The one device path, both directions: |write| flips the header's type
    /// and the data descriptor's direction -- the device reads a write's data
    /// and writes a read's.
    [[nodiscard]]
    auto submit(u64 const lba, usize const count, bool const write) -> Status {
        if (count == 0 || DATA_OFFSET + count * SECTOR + 1 > REGION_BYTES) {
            return Status::InvArgs;
        }
        auto *const bytes = reinterpret_cast<volatile u8 *>(m_region_map);
        // The header: a read (type 0) or a write (type 1), from |lba|.
        *reinterpret_cast<volatile u32 *>(bytes + REQUEST_OFFSET) = write ? 1 : 0;
        *reinterpret_cast<volatile u32 *>(bytes + REQUEST_OFFSET + 4) = 0;
        *reinterpret_cast<volatile u64 *>(bytes + REQUEST_OFFSET + 8) = lba;

        auto const buf_base = m_region_phys + REQUEST_OFFSET;
        auto const put64 = [&](usize index, usize field, u64 value) -> void {
            *reinterpret_cast<volatile u64 *>(bytes + index * 16 + field) = value;
        };
        auto const put32 = [&](usize index, usize field, u32 value) -> void {
            *reinterpret_cast<volatile u32 *>(bytes + index * 16 + field) = value;
        };
        auto const put16 = [&](usize index, usize field, u16 value) -> void {
            *reinterpret_cast<volatile u16 *>(bytes + index * 16 + field) = value;
        };
        // desc[0]: the header, read by the device, chained to desc[1].
        put64(0, 0, buf_base);
        put32(0, 8, HEADER);
        put16(0, 12, DESC_NEXT);
        put16(0, 14, 1);
        // desc[1]: the blocks, chained to desc[2]. A write's data flows
        // toward the device, so the WRITE flag -- what the device writes --
        // is on only for a read.
        put64(1, 0, buf_base + HEADER);
        put32(1, 8, static_cast<u32>(count * SECTOR));
        put16(1, 12, DESC_NEXT | (write ? u16{0} : DESC_WRITE));
        put16(1, 14, 2);
        // desc[2]: the status byte, written by the device, end of the chain.
        // It sits right after the data, which starts HEADER into the buffer:
        // drop the HEADER and the device writes the status one header's
        // width into the data's tail, and the byte read back at the true
        // status slot is whatever the region held before.
        put64(2, 0, buf_base + HEADER + count * SECTOR);
        put32(2, 8, 1);
        put16(2, 12, DESC_WRITE);
        put16(2, 14, 0);

        // The available ring: one offered chain, the device's cursor moving
        // past it. The entry lands at |m_offered| modulo the queue's size;
        // the ring's index is a free-running counter, wrapping at 16 bits
        // and never at the queue's size -- the device reads it with its own
        // free-running cursor, and an index that resets at the queue size
        // reads as the ring moving backward, its chains walked again past
        // the end. x86 orders stores strongly; no barrier is asked for.
        auto *const avail = bytes + AVAIL_OFFSET;
        *reinterpret_cast<volatile u16 *>(avail + 4 + 2 * (m_offered % LEGACY_QUEUE_NUM)) = 0;
        *reinterpret_cast<volatile u16 *>(avail + 2) = static_cast<u16>(m_offered + 1);
        m_offered += 1;
        m_regs.notify(0);

        // The device raises the line when the chain's entry lands in the used
        // ring; the wait parks the thread until it does. A level line stays
        // masked from its delivery until its ack, and each wait is also the
        // ack of the one before it, so the wait that comes back from request
        // N is what arms the line for request N + 1. Reading the ISR register
        // after each wait clears the device's own level, without which the
        // unmasked line would re-fire before any new completion. A wait can
        // come back without this request's entry -- an interrupt the last
        // request left pending -- so the entry is what ends the loop, not the
        // wait.
        auto *const used = bytes + USED_OFFSET;
        for (auto waits = u32{0}; waits < 100; ++waits) {
            if (auto const waited = ours::interrupt_wait(m_interrupt, nullptr);
                waited != Status::Ok) {
                return waited;
            }
            (void)m_regs.isr();
            if (*reinterpret_cast<volatile u16 const *>(used + 2) == static_cast<u16>(m_seen + 1)) {
                m_seen += 1;
                auto const status_byte = *reinterpret_cast<volatile u8 const *>(
                    bytes + REQUEST_OFFSET + HEADER + count * SECTOR);
                return status_byte == 0 ? Status::Ok : Status::BadState;
            }
        }
        return Status::TimeOut;
    }

  private:
    LegacyRegs m_regs = LegacyRegs{0};
    ours::VirtAddr m_region_map = ours::VirtAddr{0};
    ours::PhysAddr m_region_phys = ours::PhysAddr{0};
    /// The interrupt object on the device's line, which every submit() parks
    /// its thread on.
    ours::Uandle m_interrupt = ours::Uandle::Invalid;
    /// The chains offered so far, and the used entries seen so far: the two
    /// cursors of the ring, the device's behind the driver's by exactly the
    /// requests in flight.
    u64 m_offered = 0;
    u64 m_seen = 0;
};

/// The server: the block-driver library's BlockServer over this device. The
/// library owns the connections, the decode, the slot table and the reply;
/// this class is the hardware, the two hooks the library calls.
///
/// MAX_INFLIGHT is 1, and the work runs to completion inside start(): the
/// region holds one request's layout, so the poll of the used ring -- the
/// same one the private server did -- sits in the driver's hook, and the
/// slot is free again by the time start() returns. The library's async path
/// waits on the kernel to deliver an interrupt into a loop::Loop, which no
/// path does yet (docs/repairs/261103/kernel-interrupts.md); this driver's
/// single-request region keeps it synchronous either way, and the host
/// suite pins the async path the library provides.
class VirtioBlockServer final
    : public block_server::BlockServer<VirtioBlockServer, MAX_CLIENTS, MAX_INFLIGHT> {
  public:
    using Base = block_server::BlockServer<VirtioBlockServer, MAX_CLIENTS, MAX_INFLIGHT>;
    using Storage = typename Base::Storage;

    VirtioBlockServer(Storage &storage, loop::Loop &loop, Device &device, ours::VirtAddr region_map)
        : Base(storage, loop), m_device(&device), m_region_map(region_map) {}

    /// GetInfo's answer: the legacy device's block is a 512-byte sector, and
    /// the geometry is what setup read from the device.
    [[nodiscard]]
    auto geometry() const -> block_server::Geometry {
        return {.block_size = ours_block::BLOCK_SIZE, .blocks = m_device->blocks()};
    }

    /// The library's hand-off: run the parked request against the device,
    /// and complete it before returning. |data| is a Write's blocks, placed
    /// where the device reads them from -- the one copy of a Write.
    auto start(usize const index, std::span<u8 const> const data) -> Status {
        auto const &parked = slot(index);
        if (data.empty()) {
            // A Read: the device writes the blocks into the region's data
            // area, where the completion's encoder reads them from -- no
            // copy but the one into the reply.
            auto const status = m_device->read_blocks(parked.lba, parked.count);
            if (status != Status::Ok) {
                return status;
            }
            complete(index, Status::Ok,
                     std::span{reinterpret_cast<u8 const *>(m_region_map + REQUEST_OFFSET + HEADER),
                               usize{parked.count} * SECTOR});
            return Status::Ok;
        }
        std::memcpy(reinterpret_cast<void *>(m_region_map + REQUEST_OFFSET + HEADER), data.data(),
                    data.size());
        auto const status = m_device->write_blocks(parked.lba, parked.count);
        if (status != Status::Ok) {
            return status;
        }
        complete(index, Status::Ok);
        return Status::Ok;
    }

  private:
    Device *m_device;
    /// The region's CPU mapping, where the device leaves the data a Read
    /// answers with.
    ours::VirtAddr m_region_map = ours::VirtAddr{0};
};
} // namespace

namespace {
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<VirtioBlockServer::Storage> SERVER_STORAGE;
constinit ustl::LazyInit<Device> DEVICE_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
} // namespace

auto main(int, char **) -> int {
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (vma == Uandle::Invalid) {
        auto _ = ort::debug_line("virtio-blk: started without VmaRoot");
        return 1;
    }
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("virtio-blk: started without a DirectoryRequest handle");
        return 1;
    }
    auto const mmio_resource = ours_resource::get(ours_resource::MMIO_PROTOCOL);
    if (mmio_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no MmIo resource: ", ours::to_string(mmio_resource.unwrap_err())});
        return 1;
    }
    auto const ioport_resource = ours_resource::get(ours_resource::IOPORT_PROTOCOL);
    if (ioport_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Ioport resource: ", ours::to_string(ioport_resource.unwrap_err())});
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }
    auto const iommu_resource = ours_resource::get(ours_resource::IOMMU_PROTOCOL);
    if (iommu_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Iommu resource: ", ours::to_string(iommu_resource.unwrap_err())});
        auto _ = ours::handle_close(ioport_resource.unwrap());
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }
    auto const irq_resource = ours_resource::get(ours_resource::IRQ_PROTOCOL);
    if (irq_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Irq resource: ", ours::to_string(irq_resource.unwrap_err())});
        auto _ = ours::handle_close(iommu_resource.unwrap());
        auto _ = ours::handle_close(ioport_resource.unwrap());
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }

    // Bus 0 of the ECAM, one window for the whole enumeration.
    auto config_base = ours::VirtAddr{0};
    if (auto const status =
            map_mmio(vma, mmio_resource.unwrap(), ECAM_BASE, BUS_BYTES, &config_base);
        status != Status::Ok) {
        auto _ =
            ort::debug_line({"virtio-blk: cannot map the ECAM window: ", ours::to_string(status)});
        auto _ = ours::handle_close(iommu_resource.unwrap());
        auto _ = ours::handle_close(ioport_resource.unwrap());
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }

    // The block device, and the I/O base its legacy BAR names.
    auto device_offset = usize{0};
    auto io_base = u16{0};
    for (auto device = u8{0}; device < 32 && device_offset == 0; ++device) {
        auto const cfg = Config{config_base, to_function_offset(device)};
        auto const vendor = cfg.read16(CFG_VENDOR_ID);
        if (vendor == 0xFFFF) {
            continue;
        }
        auto const device_id = cfg.read16(CFG_DEVICE_ID);
        if (vendor != VIRTIO_VENDOR ||
            (device_id != VIRTIO_BLK_TRANSITIONAL && device_id != VIRTIO_BLK_MODERN)) {
            continue;
        }
        // The legacy BAR0: bit 0 set names I/O space, and the base is what
        // the register holds above the two low bits the specification keeps
        // for its own flags.
        auto const bar0 = cfg.read32(CFG_BAR_0);
        if ((bar0 & 1) == 0) {
            auto _ = ort::debug_line("virtio-blk: the device has no legacy I/O BAR");
            continue;
        }
        device_offset = to_function_offset(device);
        io_base = static_cast<u16>(bar0 & 0xFFFC);
    }
    auto _ = ours::vma_unmap(vma, config_base, BUS_BYTES);
    auto _ = ours::handle_close(mmio_resource.unwrap());
    if (device_offset == 0) {
        // A boot without the device attached is an answer, not a failure: the
        // boot check runs on images whose QEMU carries no disk. The routes to
        // this component's protocol answer NotFound while it is gone.
        auto _ = ort::debug_line("virtio-blk: no virtio block device on bus 0");
        auto _ = ours::handle_close(irq_resource.unwrap());
        auto _ = ours::handle_close(iommu_resource.unwrap());
        auto _ = ours::handle_close(ioport_resource.unwrap());
        return 0;
    }

    // The device, brought up once and served forever.
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"virtio-blk: cannot make its port: ", ours::to_string(status)});
        auto _ = ours::handle_close(iommu_resource.unwrap());
        auto _ = ours::handle_close(ioport_resource.unwrap());
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);

    // Everything the device owns, and the one place that puts it down. An
    // error path past the pin must stop the device and observe it stopped
    // before unpinning, or the kernel quarantines the memory.
    Uandle iommu = Uandle::Invalid;
    Uandle bti = Uandle::Invalid;
    Uandle region_vmo = Uandle::Invalid;
    Uandle region_pmt = Uandle::Invalid;
    Uandle interrupt = Uandle::Invalid;
    // Whether the root of the interrupt lines is still held: it is closed
    // once the line is taken, and a second close of a value the kernel may
    // have handed out again is not a no-op to risk.
    bool irq_root_held = true;
    ours::VirtAddr region_map = ours::VirtAddr{0};
    auto fail = [&](Status const status) -> int {
        if (region_pmt != Uandle::Invalid) {
            auto _ = ours::pmt_unpin(region_pmt);
        }
        if (region_map != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, region_map, REGION_BYTES);
        }
        if (region_vmo != Uandle::Invalid) {
            auto _ = ours::handle_close(region_vmo);
        }
        if (bti != Uandle::Invalid) {
            auto _ = ours::handle_close(bti);
        }
        if (iommu != Uandle::Invalid) {
            auto _ = ours::handle_close(iommu);
        }
        if (interrupt != Uandle::Invalid) {
            auto _ = ours::handle_close(interrupt);
        }
        if (irq_root_held) {
            irq_root_held = false;
            auto _ = ours::handle_close(irq_resource.unwrap());
        }
        auto _ = ours::handle_close(iommu_resource.unwrap());
        auto _ = ours::handle_close(ioport_resource.unwrap());
        auto _ = ort::debug_line({"virtio-blk: failed: ", ours::to_string(status)});
        return 1;
    };

    if (auto const status = ours::ioports_request(ioport_resource.unwrap(), io_base, 0x80);
        status != Status::Ok) {
        return fail(status);
    }
    auto const regs = LegacyRegs{io_base};

    // The device's interrupt line. On this machine's chipset a function at
    // root-bus slot S, pin P, is wired to GSI 20 + ((S + P) & 3), and QEMU
    // adds the disk's virtio-blk as the first PCI function, slot 3 pin A:
    // GSI 23. The firmware's tables name the same wiring, and the route
    // service planned for them will name the line from those instead of this
    // constant. PCI INTx is level and active low, which ModeLevelLow says.
    // ACPI's numbering is the root domain's, which interrupt_map resolves
    // from.
    auto line_number = u32{0};
    if (auto const status = ours::interrupt_map(irq_resource.unwrap(), ours::ROOT_INTERRUPT_DOMAIN,
                                                GSI_DISK, &line_number);
        status != Status::Ok) {
        return fail(status);
    }
    if (auto const status = ours::interrupt_create(
            irq_resource.unwrap(), line_number, ours::InterruptOptions::ModeLevelLow, &interrupt);
        status != Status::Ok) {
        return fail(status);
    }
    // The root is only the means of mapping and taking: the line is held now,
    // and nothing else is asked of the root. Closing it lets the next driver
    // reach the resource, as the keyboard's driver does with its line.
    irq_root_held = false;
    auto _ = ours::handle_close(irq_resource.unwrap());

    // Bring-up, in the order the legacy specification fixes.
    regs.set_status(0);
    for (auto tries = u32{0}; tries < 1'000'000; ++tries) {
        if (regs.status() == 0) {
            break;
        }
        ort::spin_hint();
    }
    regs.set_status(STATUS_ACKNOWLEDGE);
    regs.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    (void)regs.host_features();
    regs.accept_no_features();

    regs.select_queue(0);
    auto const queue_max = regs.queue_num();
    if (queue_max == 0) {
        return fail(Status::OutOfRange);
    }
    // The legacy depth is the driver's to choose, within the device's
    // maximum: written back before the ring is registered, so the device
    // computes the rings' addresses from the same depth this driver laid
    // them out with.
    regs.set_queue_num(static_cast<u16>(LEGACY_QUEUE_NUM));

    if (auto const status =
            ours::iommu_create(iommu_resource.unwrap(), ours::IommuType::Stub, nullptr, 0, &iommu);
        status != Status::Ok) {
        return fail(status);
    }
    if (auto const status = ours::bti_create(iommu, 0, 0, &bti); status != Status::Ok) {
        return fail(status);
    }
    if (auto const status = ours::vmo_create_contiguous(bti, REGION_BYTES, 0, &region_vmo);
        status != Status::Ok) {
        return fail(status);
    }
    auto region_phys = ours::PhysAddr{0};
    if (auto const status =
            ours::bti_pin(bti,
                          ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite |
                              ours::BtiPinOptions::Contiguous,
                          region_vmo, 0, REGION_BYTES, &region_phys, 1, &region_pmt);
        status != Status::Ok) {
        return fail(status);
    }
    if (auto const status =
            ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0,
                          region_vmo, 0, REGION_BYTES, &region_map);
        status != Status::Ok) {
        return fail(status);
    }
    regs.set_queue_pfn(region_phys);
    {
        char phys_text[18];
        auto _ = ort::debug_line({"virtio-blk: region at ", ort::hex(region_phys, phys_text)});
    }

    // The device is told the driver is ready, and may now be served.
    regs.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

    // The server, over the device.
    auto &storage = *SERVER_STORAGE.init();
    auto &device = *DEVICE_STORAGE.init();
    device.init(io_base, region_map, region_phys, interrupt);
    VirtioBlockServer server(storage, loop, device, region_map);

    auto &outgoing_storage = *OUTGOING_STORAGE.init();
    ioservice::Outgoing outgoing(outgoing_storage, loop);
    if (auto const status = outgoing.add_protocol(ours_block::BLOCK_NAME, server);
        status != Status::Ok) {
        return fail(status);
    }
    if (auto const status = outgoing.serve(directory_request); status != Status::Ok) {
        return fail(status);
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line({"virtio-blk: serving ", ours_block::BLOCK_NAME});
    auto _ = ort::debug_line(
        {"virtio-blk: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
