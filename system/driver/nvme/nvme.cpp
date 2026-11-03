/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// nvme -- the userspace driver of QEMU's NVMe controller (storage-layer
/// plan, system/driver/nvme), as a component that serves
/// ours.block.Block: a client opens the protocol, asks for the namespace's
/// geometry or for blocks by number, and the driver drives the controller
/// on its behalf. Root.cm offers it under the name ours.block.Nvme, so a
/// machine with both disks answers on two names.
///
/// The device is found in the ECAM window by class code (01h/08h, mass
/// storage / NVM controller) rather than by vendor and device: the class
/// names the family, and every vendor's controller is driven the same way.
/// BAR0 is 16 KiB of MMIO -- the register file: CAP, CC, CSTS, the admin
/// and I/O queue doorbells -- mapped uncached through the MmIo root, and
/// every control is a store through that mapping.
///
/// The objects:
///   - the MmIo root resource and the IOMMU System resource, through /svc:
///     the window mapping and the pin that hands the queues to the device;
///   - the DMA region: one contiguous run of pages from
///     vmo_create_contiguous, hand-sliced at page boundaries -- the admin
///     queues, the Identify page, the I/O queues, and one page per slot
///     for the blocks, which is what PRP wants (PRP1 = the page, PRP2 = 0,
///     a 7-block transfer fitting one page);
///   - the Loop, on which the outgoing directory and every client
///     connection wait;
///   - the server, block_server::BlockServer over the controller: the
///     library owns the connections, the decode, the slot table, and the
///     reply; the driver's start() builds the 64-byte command, writes it
///     to the I/O submission queue, rings the doorbell, and polls the
///     completion queue's phase bit until this command's entry lands --
///     the poll is on the same thread as everything else, so requests
///     serialize, which one I/O queue makes true anyway. The device's
///     interrupt pin is never unmasked: polling needs no line, and the
///     kernel's MSI does not exist yet.
///
/// The events: the root routes a client's Open into the outgoing directory
/// (design D11); the server takes the channel and parks the request in a
/// slot; start() builds the command (opcode Read or Write, NSID 1, CID =
/// the slot), submits, polls the completion, and complete() turns it into
/// the reply. Before the driver leaves, CC.EN is cleared and CSTS.RDY
/// observed low, so the kernel never has to quarantine what it pinned.
///
/// QEMU's controller: attached with NVME_DISK= (scripts/qemu_boot.py), it
/// lands somewhere on bus 0 of the ECAM window the route service names; a
/// boot without it finds no class-01h/08h function and exits 0 -- an
/// answer, not a failure, as virtio-blk's no-disk path is.
#include <ours/bti.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/time.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <string_view>

#include <cstdint>
#include <cstring>

#include <block_server/block_server.hpp>
#include <ddk/ddk.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <ours_acpi/client.hpp>
#include <ours_block/ours_block.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The ECAM walk's geometry: one 4 KiB function page per
/// bus-slot-function triple, all of bus 0 mapped through one window.
constexpr auto DEVICE_SHIFT = usize{15};

/// The PCI configuration registers the walk reads and, for the device the
/// walk claims, the command register it writes: PCI command bit 2 (bus
/// master) gates the device's own DMA, and a boot firmware that probed the
/// device and gave up on it may have left the bit clear.
constexpr auto CFG_VENDOR_ID = usize{0x00};
constexpr auto CFG_COMMAND = usize{0x04};
constexpr auto CFG_CLASS_BYTE = usize{0x0B};
constexpr auto CFG_SUBCLASS_BYTE = usize{0x0A};
constexpr auto CFG_BAR_0 = usize{0x10};
constexpr auto CFG_BAR_0_HIGH = usize{0x14};

/// A mass-storage controller whose specific subclass is the NVM one: class
/// base 01h, subclass 08h names every vendor's NVMe.
constexpr auto CLASS_MASS_STORAGE = u8{0x01};
constexpr auto SUBCLASS_NVM = u8{0x08};

/// The register file's layout, at byte offsets from BAR0.
constexpr auto NVME_CAP = usize{0x00};
constexpr auto NVME_VS = usize{0x08};
constexpr auto NVME_CC = usize{0x14};
constexpr auto NVME_CSTS = usize{0x1C};
constexpr auto NVME_AQA = usize{0x24};
constexpr auto NVME_ASQ = usize{0x28};
constexpr auto NVME_ACQ = usize{0x30};
/// The doorbell array's base: queue pair |qid| rings its SQ tail at
/// NVME_SQ0TDBL + 2*qid*stride and its CQ head one stride further, where
/// stride is CAP.DSTRD's (1 << (2 + DSTRD)); the admin pair is qid 0.
constexpr auto NVME_SQ0TDBL = usize{0x1000};

/// CC's bits: enabled, and the I/O queue entry sizes in log2 units (64-byte
/// SQ entries = 6, 16-byte CQE entries = 4), written before EN goes high.
constexpr auto CC_ENABLE = u64{1};
constexpr auto CC_IOSQES_SHIFT = usize{16};
constexpr auto CC_IOCQES_SHIFT = usize{20};
constexpr auto CC_IOSQES_VALUE = u64{6} << CC_IOSQES_SHIFT;
constexpr auto CC_IOCQES_VALUE = u64{4} << CC_IOCQES_SHIFT;

/// CSTS's bits: ready, and the fatal status.
constexpr auto CSTS_READY = u64{1};
constexpr auto CSTS_FATAL = u64{1} << 1;

/// The admin queue depths: 2 entries each carries bring-up's commands, run
/// one at a time. AQA packs both as (count - 1).
constexpr auto ADMIN_QUEUE_ENTRIES = usize{2};
/// The I/O queue pair's depth: one slot is in flight at a time, the block
/// service's tag depth, and the queue carries it with room to spare.
constexpr auto IO_QUEUE_ENTRIES = usize{4};
constexpr auto AQA_ASQS_FIELD = u32{ADMIN_QUEUE_ENTRIES - 1};
constexpr auto AQA_ACQS_FIELD = u32{ADMIN_QUEUE_ENTRIES - 1} << 16;

/// The admin command opcodes: Identify Namespace (CNS 0 names a namespace),
/// and the I/O queue pair's creation, the CQ first, for the SQ to name it.
constexpr auto ADMIN_CREATE_IO_SQ = u8{0x01};
constexpr auto ADMIN_CREATE_IO_CQ = u8{0x05};
constexpr auto ADMIN_IDENTIFY = u8{6};
constexpr auto CNS_IDENTIFY_NAMESPACE = u32{0};
/// The I/O command opcodes, NVM command set.
constexpr auto IO_WRITE = u8{0x01};
constexpr auto IO_READ = u8{0x02};
/// The one namespace this driver claims, and the I/O queues' id: 0 is the
/// admin pair's.
constexpr auto NSID = u32{1};
constexpr auto QID_IO = u16{1};

/// DMA layout: every slice page-aligned, because PRP wants pages, and the
/// slices' offsets are the driver's constant map of the region. One data
/// page per in-flight request: the largest read, 7 blocks, fits a page, so
/// PRP1 alone names the buffer and PRP2 stays zero.
constexpr auto PAGE = usize{4096};
constexpr auto ADMIN_SQ_BYTES = 2 * PAGE;
constexpr auto ADMIN_CQ_BYTES = 2 * PAGE;
constexpr auto IDENTIFY_BYTES = PAGE;
constexpr auto IO_SQ_BYTES = 2 * PAGE;
constexpr auto IO_CQ_BYTES = 2 * PAGE;
constexpr auto DATA_BYTES = PAGE;
constexpr auto DMA_BYTES =
    ADMIN_SQ_BYTES + ADMIN_CQ_BYTES + IDENTIFY_BYTES + IO_SQ_BYTES + IO_CQ_BYTES + DATA_BYTES;
constexpr auto SLICE_ADMIN_SQ = usize{0};
constexpr auto SLICE_ADMIN_CQ = SLICE_ADMIN_SQ + ADMIN_SQ_BYTES;
constexpr auto SLICE_IDENTIFY = SLICE_ADMIN_CQ + ADMIN_CQ_BYTES;
constexpr auto SLICE_IO_SQ = SLICE_IDENTIFY + IDENTIFY_BYTES;
constexpr auto SLICE_IO_CQ = SLICE_IO_SQ + IO_SQ_BYTES;
constexpr auto SLICE_DATA = SLICE_IO_CQ + IO_CQ_BYTES;

/// The 64-byte submission queue entry. Only the fields the commands here
/// use are named; the rest stay zero, which the specification requires for
/// "not used".
struct SqEntry final {
    u8 opcode = 0;
    u8 flags = 0;
    u16 command_id = 0;
    u32 nsid = 0;
    u32 reserved0 = 0;
    u64 mptr = 0;
    u64 prp1 = 0;
    u64 prp2 = 0;
    u32 cdw10 = 0;
    u32 cdw11 = 0;
    u32 cdw12 = 0;
    u32 reserved13 = 0;
    u32 cdw14 = 0;
    u32 reserved15 = 0;
};
static_assert(sizeof(SqEntry) == 64);

/// The 16-byte completion queue entry, as the device lays it out. The
/// third dword names the submission queue's consumed head and its id; the
/// fourth carries the command id in its low half, the phase at bit 16 --
/// the bit a polled driver tells a fresh entry from a stale one by, which
/// flips each pass of the ring -- and the status field above, whose clear
/// value names success.
struct CqEntry final {
    u32 command_specific = 0;
    u32 reserved = 0;
    u32 word2 = 0;
    u32 word3 = 0;
};
static_assert(sizeof(CqEntry) == 16);

/// Whether the completion |dw3| names: the status field, the phase bit
/// aside, is zero exactly when the command succeeded.
[[nodiscard]]
constexpr auto command_ok(u32 const dw3) -> bool {
    return (dw3 & 0xFFFE'0000u) == 0;
}

/// MMIO reads and writes through the controller's mapping, volatile
/// because the device acts on every store.
class Regs final {
  public:
    explicit Regs(ours::VirtAddr base) : m_base(base) {}

    [[nodiscard]]
    auto read64(usize const at) const -> u64 {
        return *reinterpret_cast<volatile u64 const *>(m_base + at);
    }
    [[nodiscard]]
    auto read32(usize const at) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_base + at);
    }
    auto write64(usize const at, u64 const value) const -> void {
        *reinterpret_cast<volatile u64 *>(m_base + at) = value;
    }
    auto write32(usize const at, u32 const value) const -> void {
        *reinterpret_cast<volatile u32 *>(m_base + at) = value;
    }

    /// CAP.DSTRD: the byte stride between consecutive doorbell registers,
    /// 1 << (2 + DSTRD) -- 4 on QEMU, SQ0's tail at 0x1000 and CQ0's head
    /// one stride after.
    [[nodiscard]]
    auto doorbell_stride() const -> usize {
        return usize{1} << (((read32(NVME_CAP + 4) >> 24) & 0xF) + 2);
    }

  private:
    ours::VirtAddr m_base;
};

/// The controller: the register file and the DMA slices, with the views
/// the admin commands and the block service read through. One controller,
/// one namespace, one I/O queue pair; commands run one at a time.
class Controller final {
  public:
    Controller(ours::PhysAddr dma_phys, ours::VirtAddr dma_virt)
        : m_dma_phys(dma_phys), m_dma_virt(dma_virt) {}

    [[nodiscard]]
    auto admin_sq_phys() const -> ours::PhysAddr {
        return m_dma_phys + SLICE_ADMIN_SQ;
    }
    [[nodiscard]]
    auto admin_cq_phys() const -> ours::PhysAddr {
        return m_dma_phys + SLICE_ADMIN_CQ;
    }
    [[nodiscard]]
    auto admin_sq() const -> ours::VirtAddr {
        return m_dma_virt + SLICE_ADMIN_SQ;
    }
    [[nodiscard]]
    auto admin_cq() const -> ours::VirtAddr {
        return m_dma_virt + SLICE_ADMIN_CQ;
    }
    [[nodiscard]]
    auto identify() const -> ours::VirtAddr {
        return m_dma_virt + SLICE_IDENTIFY;
    }
    /// The in-flight request's buffer: the one data page, aligned as PRP
    /// wants, where a Read's blocks land and a Write's start from. One page
    /// carries the largest read -- 7 blocks -- and the tag depth is 1, so
    /// the request in flight always owns it.
    [[nodiscard]]
    auto data() const -> ours::VirtAddr {
        return m_dma_virt + SLICE_DATA;
    }
    [[nodiscard]]
    auto data_phys() const -> ours::PhysAddr {
        return m_dma_phys + SLICE_DATA;
    }
    [[nodiscard]]
    auto io_sq_phys() const -> ours::PhysAddr {
        return m_dma_phys + SLICE_IO_SQ;
    }
    [[nodiscard]]
    auto io_cq_phys() const -> ours::PhysAddr {
        return m_dma_phys + SLICE_IO_CQ;
    }
    [[nodiscard]]
    auto io_sq() const -> ours::VirtAddr {
        return m_dma_virt + SLICE_IO_SQ;
    }
    [[nodiscard]]
    auto io_cq() const -> ours::VirtAddr {
        return m_dma_virt + SLICE_IO_CQ;
    }

  private:
    ours::PhysAddr m_dma_phys;
    ours::VirtAddr m_dma_virt;
};

/// One submission/completion queue pair, polled. The admin pair and the
/// I/O pair differ in their DMA slices, their depths and their doorbells
/// alone, so one class serves both: the doorbells are byte offsets in the
/// register file, computed by the pair's queue id as the doorbell array's
/// layout names them.
class Queues final {
  public:
    Queues(Regs const &regs, ours::VirtAddr sq, ours::VirtAddr cq, usize const sq_doorbell,
           usize const cq_doorbell, usize const entries)
        : m_regs(regs)
        , m_sq(sq)
        , m_cq(cq)
        , m_sq_doorbell(sq_doorbell)
        , m_cq_doorbell(cq_doorbell)
        , m_entries(entries) {}

    /// Submits the command |entry| names, stamped with the command id |id|
    /// the completion is matched by, polls for that completion, and hands
    /// the entry back. False on timeout. The expected phase starts true:
    /// the host zeroes every entry's phase bit at creation, the controller
    /// writes 1 on the first pass, and the expected phase toggles each full
    /// pass of the ring -- so a fresh entry's bit matches the expected one,
    /// and a stale entry's differs.
    [[nodiscard]]
    auto submit(SqEntry entry, u16 const id, CqEntry &out) -> bool {
        entry.command_id = id;
        auto *const slot = reinterpret_cast<volatile SqEntry *>(m_sq) + m_tail;
        // The whole 64-byte entry, written field by field: volatile
        // destinations reject the struct copy.
        auto const *const src = &entry;
        auto *const dst = reinterpret_cast<volatile u32 *>(slot);
        for (auto word = usize{0}; word < sizeof(SqEntry) / 4; ++word) {
            u32 value = 0;
            std::memcpy(&value, reinterpret_cast<u8 const *>(src) + word * 4, 4);
            dst[word] = value;
        }
        m_tail = (m_tail + 1) % m_entries;
        // The doorbell takes the new tail: the index one past the last
        // entry the device has not consumed, which a fresh submission makes
        // this entry's own slot.
        m_regs.write32(m_sq_doorbell, static_cast<u32>(m_tail));
        return wait_for(id, out);
    }

  private:
    /// Polls the completion queue for the entry whose command id is |id|,
    /// the phase bit naming it fresh. The head doorbell rings after.
    auto wait_for(u16 const id, CqEntry &out) -> bool {
        auto *const entries = reinterpret_cast<volatile CqEntry *>(m_cq);
        for (auto round = u32{0}; round < POLL_ROUNDS; ++round) {
            auto const &at = entries[m_head];
            auto const phase = (at.word3 & 0x1'0000u) != 0;
            if (phase != m_phase) {
                continue;
            }
            if ((at.word3 & 0xFFFFu) == id) {
                out = CqEntry{at.command_specific, at.reserved, at.word2, at.word3};
                m_head = (m_head + 1) % m_entries;
                if (m_head == 0) {
                    m_phase = !m_phase;
                }
                // The completion head doorbell: the controller may reuse
                // the entry only once the host has rung.
                m_regs.write32(m_cq_doorbell, static_cast<u32>(m_head));
                return true;
            }
            ort::spin_hint();
        }
        return false;
    }

    Regs m_regs;
    ours::VirtAddr m_sq;
    ours::VirtAddr m_cq;
    usize m_sq_doorbell = 0;
    usize m_cq_doorbell = 0;
    usize m_entries = 0;
    u16 m_tail = 0;
    u16 m_head = 0;
    bool m_phase = true;
    static constexpr auto POLL_ROUNDS = u32{4'000'000};
};

/// Spins |predicate| until true or |seconds| pass, printing a line every
/// second: the wait is silent work on a console whose 15-second silence
/// trips the boot check's NMI, so it must keep talking; and the wait is
/// wall-clock because the controller's ready transitions run on the
/// device's own timers (CAP.TO, 500 ms units -- 7.5 s on QEMU), which
/// advance at the emulator's pace, not the driver's.
template <typename Predicate>
auto wait_silent(Predicate &&predicate, u64 const seconds, char const *const what) -> bool {
    auto const started = ours::clock_get_monotonic();
    auto printed = u64{0};
    for (;;) {
        if (predicate()) {
            return true;
        }
        auto const now = ours::clock_get_monotonic();
        auto const elapsed = static_cast<u64>((now - started) / 1'000'000);
        if (elapsed > printed) {
            printed = elapsed;
            if (elapsed >= seconds) {
                return false;
            }
            char text[10];
            auto _ = ort::debug_line({"nvme: waiting for ", what, " (",
                                      ort::decimal(static_cast<u32>(elapsed), text), "s)"});
        }
        ort::spin_hint();
    }
}

/// The config-space reader over the mapped ECAM window, one function's
/// page: the walk reads vendor, class and BAR0 from it.
class Config final {
  public:
    Config(ours::VirtAddr base, usize const offset) : m_at(base + offset) {}

    [[nodiscard]]
    auto read8(usize const at) const -> u8 {
        return *reinterpret_cast<volatile u8 const *>(m_at + at);
    }
    [[nodiscard]]
    auto read16(usize const at) const -> u16 {
        return *reinterpret_cast<volatile u16 const *>(m_at + at);
    }
    [[nodiscard]]
    auto read32(usize const at) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_at + at);
    }
    auto write32(usize const at, u32 const value) const -> void {
        *reinterpret_cast<volatile u32 *>(m_at + at) = value;
    }

  private:
    ours::VirtAddr m_at;
};

/// The block service's shape: two clients at once, and the device works on
/// one request at a time -- the poll model, MAX_INFLIGHT the tag depth the
/// one data page answers.
constexpr auto MAX_CLIENTS = usize{2};
constexpr auto MAX_INFLIGHT = usize{1};

/// The block service over the polled controller, the driver's subclass of
/// the block-driver library: GetInfo answers the Identify'd geometry, and
/// start() runs one parked request to completion -- the 64-byte command
/// built, the data page primed for a Write, the doorbell rung, the
/// completion polled, the slot completed -- so the slot is free again by
/// the time start() returns, as virtio-blk's synchronous start() is. One
/// queue pair and one data page make the serialization physical.
class NvmeBlockServer final
    : public block_server::BlockServer<NvmeBlockServer, MAX_CLIENTS, MAX_INFLIGHT> {
  public:
    using Base = block_server::BlockServer<NvmeBlockServer, MAX_CLIENTS, MAX_INFLIGHT>;
    using Storage = typename Base::Storage;

    NvmeBlockServer(Storage &storage, loop::Loop &loop, Controller &controller, Queues &io)
        : Base(storage, loop), m_controller(&controller), m_io(&io) {}

    auto set_blocks(u64 const blocks) -> void {
        m_blocks = blocks;
    }

    /// GetInfo's answer: the namespace's logical block is the 512-byte
    /// sector ours.block names, and the count is what Identify returned.
    [[nodiscard]]
    auto geometry() const -> block_server::Geometry {
        return {.block_size = ours_block::BLOCK_SIZE, .blocks = m_blocks};
    }

    /// The library's hand-off: run the parked request against the device,
    /// and complete it before returning. |data| is a Write's blocks, copied
    /// into the data page the command's PRP1 names -- the one copy of a
    /// Write, as the region-backed virtio path copies once too.
    auto start(usize const index, std::span<u8 const> const data) -> Status {
        auto const &parked = slot(index);
        auto entry = SqEntry{};
        entry.opcode = data.empty() ? IO_READ : IO_WRITE;
        entry.nsid = NSID;
        entry.prp1 = m_controller->data_phys();
        // SLBA, the first block, is 64 bits over CDW10 and CDW11; NLB, the
        // count, is zero-based.
        entry.cdw10 = static_cast<u32>(parked.lba);
        entry.cdw11 = static_cast<u32>(parked.lba >> 32);
        entry.cdw12 = parked.count - 1;
        if (!data.empty()) {
            std::memcpy(reinterpret_cast<void *>(m_controller->data()), data.data(), data.size());
        }
        auto cqe = CqEntry{};
        if (!m_io->submit(entry, static_cast<u16>(index), cqe)) {
            return Status::TimeOut;
        }
        // Success is the whole status field clear -- its position the
        // device's own encoding, which a failure here says on the console
        // instead of this driver guessing wrong about.
        if (!command_ok(cqe.word3)) {
            char status_text[18];
            auto _ = ort::debug_line(
                {"nvme: the device failed a command, DW3 ", ort::hex(cqe.word3, status_text)});
            return Status::BadState;
        }
        if (data.empty()) {
            complete(index, Status::Ok,
                     std::span{reinterpret_cast<u8 const *>(m_controller->data()),
                               usize{parked.count} * ours_block::BLOCK_SIZE});
        } else {
            complete(index, Status::Ok);
        }
        return Status::Ok;
    }

  private:
    Controller *m_controller;
    Queues *m_io;
    u64 m_blocks = 0;
};

constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<NvmeBlockServer::Storage> SERVER_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};

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
    // table, as virtio-blk's driver does for its enumeration.
    auto const ecam = ours_acpi::ecam_base(0);
    if (ecam.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: no ECAM base from the route service: ", ours::to_string(ecam.unwrap_err())});
        return 1;
    }

    // The whole window: every bus the firmware named, walked function page
    // by function page.
    auto ecam_window = ddk::MappedMmio::map(vma.unwrap(), mmio_resource.unwrap(), ecam->address,
                                            usize{ecam->size});
    if (ecam_window.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: cannot map the ECAM window: ", ours::to_string(ecam_window.unwrap_err())});
        return 1;
    }
    auto const config_base = ecam_window->addr();

    // The controller, by class: base 01h mass storage, subclass 08h NVM.
    auto device_offset = usize{0};
    u64 bar0_phys = 0;
    for (auto device = u8{0}; device < 32 && device_offset == 0; ++device) {
        auto const cfg = Config{config_base, usize{device} << DEVICE_SHIFT};
        auto const vendor = cfg.read16(CFG_VENDOR_ID);
        if (vendor == 0xFFFF) {
            continue;
        }
        if (cfg.read8(CFG_CLASS_BYTE) != CLASS_MASS_STORAGE ||
            cfg.read8(CFG_SUBCLASS_BYTE) != SUBCLASS_NVM) {
            continue;
        }
        // BAR0 is a 64-bit memory BAR: the low register holds bits 0-31
        // with the low nibble the type flags (bit 0 clear names memory,
        // bits 2-1 = 10b a 64-bit width), the high register bits 32-63.
        auto const bar_low = cfg.read32(CFG_BAR_0);
        if ((bar_low & 0x1) != 0 || (bar_low & 0x6) != 0x4) {
            continue;
        }
        device_offset = device << DEVICE_SHIFT;
        bar0_phys = (u64{cfg.read32(CFG_BAR_0_HIGH)} << 32) | u64{bar_low & ~u32{0xF}};
        // The device's own DMA (queue fetch, completions) needs bus master
        // and the memory-space decode both set in the command register: a
        // boot firmware that probed the device and gave up may have left
        // them clear, and the controller then fetches no commands and never
        // reaches CSTS.RDY.
        cfg.write32(CFG_COMMAND,
                    cfg.read32(CFG_COMMAND) | 0x2 /* memory space */ | 0x4 /* bus master */);
    }
    if (device_offset == 0) {
        // A boot without the device attached is an answer, not a failure:
        // the boot check runs on images whose QEMU carries no NVMe. The
        // routes to this component's protocol answer NotFound while it is
        // gone.
        auto _ = ort::debug_line("nvme: no NVMe controller on bus 0");
        return 0;
    }

    // The controller's register file, mapped uncached.
    auto bar0 = ddk::MappedMmio::map(vma.unwrap(), mmio_resource.unwrap(),
                                     ours::PhysAddr{bar0_phys}, 16 * PAGE);
    if (bar0.is_err()) {
        auto _ = ort::debug_line({"nvme: cannot map BAR0: ", ours::to_string(bar0.unwrap_err())});
        return 1;
    }
    auto const regs_addr = bar0->addr();
    Regs regs{regs_addr};

    // The DMA region, sliced per the constants above.
    auto bti = ddk::Bti::create(iommu_resource.unwrap());
    if (bti.is_err()) {
        auto _ =
            ort::debug_line({"nvme: cannot make its BTI: ", ours::to_string(bti.unwrap_err())});
        return 1;
    }
    resources.drop(iommu_resource.unwrap());
    auto dma = ddk::DmaRegion::allocate(vma.unwrap(), bti->handle(), DMA_BYTES);
    if (dma.is_err()) {
        auto _ = ort::debug_line(
            {"nvme: cannot make its DMA region: ", ours::to_string(dma.unwrap_err())});
        return 1;
    }
    Controller controller(dma->phys(), dma->virt());

    // Bring-up, in the order the specification fixes. The waits are
    // wall-clock: the controller's ready transitions run on the device's
    // own timers (CAP.TO, 500 ms units -- 7.5 s on QEMU), and the waits
    // print per-second keep-alives so the console's silence detector does
    // not NMI the machine mid-bring-up.
    regs.write32(NVME_CC, 0);
    if (!wait_silent([&] { return (regs.read32(NVME_CSTS) & CSTS_READY) == 0; }, 30,
                     "CSTS.RDY 0")) {
        auto _ = ort::debug_line("nvme: the controller would not leave CSTS.RDY");
        return 1;
    }
    auto const stride = regs.doorbell_stride();

    // The admin queues: base addresses must be page-aligned at least, and
    // the region's slices are page-aligned.
    regs.write32(NVME_AQA, AQA_ACQS_FIELD | AQA_ASQS_FIELD);
    regs.write64(NVME_ASQ, dma->phys() + SLICE_ADMIN_SQ);
    regs.write64(NVME_ACQ, dma->phys() + SLICE_ADMIN_CQ);

    // The I/O queue entry sizes, then enable.
    auto const cc_sizes = static_cast<u32>(CC_IOSQES_VALUE | CC_IOCQES_VALUE);
    regs.write32(NVME_CC, cc_sizes);
    regs.write32(NVME_CC, cc_sizes | static_cast<u32>(CC_ENABLE));
    if (!wait_silent([&] { return (regs.read32(NVME_CSTS) & CSTS_READY) != 0; }, 30,
                     "CSTS.RDY 1")) {
        auto _ = ort::debug_line("nvme: the controller never reached CSTS.RDY");
        return 1;
    }
    if ((regs.read32(NVME_CSTS) & CSTS_FATAL) != 0) {
        auto _ = ort::debug_line("nvme: the controller raised CSTS.CFS");
        return 1;
    }

    char ver_text[18];
    char stride_text[18];
    auto _ = ort::debug_line({"nvme: controller ready, version ",
                              ort::hex(regs.read32(NVME_VS), ver_text), ", doorbell stride ",
                              ort::hex(static_cast<u32>(stride), stride_text)});

    // The admin queues, then the namespace's size, from Identify Namespace:
    // the geometry the block service answers GetInfo with.
    auto admin = Queues(regs, controller.admin_sq(), controller.admin_cq(), NVME_SQ0TDBL,
                        NVME_SQ0TDBL + stride, ADMIN_QUEUE_ENTRIES);
    auto cqe = CqEntry{};
    {
        auto entry = SqEntry{};
        entry.opcode = ADMIN_IDENTIFY;
        entry.nsid = NSID;
        entry.prp1 = dma->phys() + SLICE_IDENTIFY;
        entry.cdw10 = CNS_IDENTIFY_NAMESPACE;
        if (!admin.submit(entry, 0, cqe)) {
            auto _ = ort::debug_line("nvme: Identify timed out");
            return 1;
        }
        if (!command_ok(cqe.word3)) {
            auto _ = ort::debug_line("nvme: Identify failed");
            return 1;
        }
    }
    char nsze_text[18];
    auto const nsze = *reinterpret_cast<volatile u64 const *>(controller.identify());
    auto _ = ort::debug_line({"nvme: controller ready, NSZE ", ort::hex(nsze, nsze_text)});
    if (nsze == 0) {
        auto _ = ort::debug_line("nvme: the namespace names no blocks");
        return 0;
    }

    // The I/O queue pair, created over the admin queues -- the CQ first,
    // for the SQ to name it. Queue size is the entry count itself (AQA's
    // counts were minus one; these are not); the CQ is physically
    // contiguous, its interrupts not enabled -- the poll model -- and the
    // SQ names this CQ at priority zero, which polling ignores.
    auto io =
        Queues(regs, controller.io_sq(), controller.io_cq(), NVME_SQ0TDBL + 2 * QID_IO * stride,
               NVME_SQ0TDBL + (2 * QID_IO + 1) * stride, IO_QUEUE_ENTRIES);
    {
        auto entry = SqEntry{};
        entry.opcode = ADMIN_CREATE_IO_CQ;
        entry.prp1 = dma->phys() + SLICE_IO_CQ;
        entry.cdw10 = static_cast<u32>(IO_QUEUE_ENTRIES) << 16 | QID_IO;
        entry.cdw11 = 0x1 /* physically contiguous */;
        if (!admin.submit(entry, 1, cqe)) {
            auto _ = ort::debug_line("nvme: Create I/O CQ timed out");
            return 1;
        }
        if (!command_ok(cqe.word3)) {
            char status_text[18];
            auto _ = ort::debug_line(
                {"nvme: Create I/O CQ failed, DW3 ", ort::hex(cqe.word3, status_text)});
            return 1;
        }
        entry = SqEntry{};
        entry.opcode = ADMIN_CREATE_IO_SQ;
        entry.prp1 = dma->phys() + SLICE_IO_SQ;
        entry.cdw10 = static_cast<u32>(IO_QUEUE_ENTRIES) << 16 | QID_IO;
        entry.cdw11 = 0x1 /* physically contiguous */ | u32{QID_IO} << 16;
        if (!admin.submit(entry, 2, cqe)) {
            auto _ = ort::debug_line("nvme: Create I/O SQ timed out");
            return 1;
        }
        if (!command_ok(cqe.word3)) {
            char status_text[18];
            auto _ = ort::debug_line(
                {"nvme: Create I/O SQ failed, DW3 ", ort::hex(cqe.word3, status_text)});
            return 1;
        }
    }

    // The device is brought up and the I/O queues live: the block service
    // serves its protocol until the machine ends. The kit objects and the
    // storages outlive the Loop.
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"nvme: cannot make its port: ", ours::to_string(status)});
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    auto &storage = *SERVER_STORAGE.init();
    NvmeBlockServer server(storage, loop, controller, io);
    server.set_blocks(nsze);

    auto &outgoing_storage = *OUTGOING_STORAGE.init();
    ioservice::Outgoing outgoing(outgoing_storage, loop);
    if (auto const status = outgoing.add_protocol(ours_block::BLOCK_NAME, server);
        status != Status::Ok) {
        auto _ = ort::debug_line({"nvme: cannot serve its protocol: ", ours::to_string(status)});
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        auto _ = ort::debug_line(
            {"nvme: cannot serve its outgoing directory: ", ours::to_string(status)});
        return 1;
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line(
        {"nvme: serving ", ours_block::BLOCK_NAME, " over ", ort::hex(nsze, nsze_text), " blocks"});
    auto _ = ort::debug_line(
        {"nvme: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
