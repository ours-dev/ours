/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// nvme -- the userspace driver of QEMU's NVMe controller (storage-layer
/// plan, system/driver/nvme), as a component that provides
/// ours.block.Nvme: a client opens the protocol, asks for the namespace's
/// geometry or for blocks by number, and the driver drives the controller
/// on its behalf.
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

#include <ddk/ddk.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <ours_acpi/client.hpp>
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
/// The doorbells: SQ0's tail at 0x1000, CQ0's head one stride after.
constexpr auto NVME_SQ0TDBL = usize{0x1000};
/// The doorbells: SQ0's tail at 0x1000, CQ0's head DSTRD bytes after.

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

/// The admin queue depths: 2 entries each carries the four commands
/// bring-up runs, one at a time. AQA packs both as (count - 1).
constexpr auto ADMIN_QUEUE_ENTRIES = usize{2};
constexpr auto ADMIN_IDENTIFY = u8{6};
constexpr auto CNS_IDENTIFY_NAMESPACE = u32{0};
constexpr auto AQA_ASQS_FIELD = u32{ADMIN_QUEUE_ENTRIES - 1};
constexpr auto AQA_ACQS_FIELD = u32{ADMIN_QUEUE_ENTRIES - 1} << 16;

/// The admin command opcodes, and Identify's CNS value for a namespace.

/// The I/O command opcodes.

/// DMA layout: every slice page-aligned, because PRP wants pages, and the
/// slices' offsets are the driver's constant map of the region.
constexpr auto PAGE = usize{4096};
constexpr auto ADMIN_SQ_BYTES = 2 * PAGE;
constexpr auto ADMIN_CQ_BYTES = 2 * PAGE;
constexpr auto IDENTIFY_BYTES = PAGE;
constexpr auto IO_SQ_BYTES = 2 * PAGE;
constexpr auto IO_CQ_BYTES = 2 * PAGE;
constexpr auto DMA_BYTES =
    ADMIN_SQ_BYTES + ADMIN_CQ_BYTES + IDENTIFY_BYTES + IO_SQ_BYTES + IO_CQ_BYTES;
constexpr auto SLICE_ADMIN_SQ = usize{0};
constexpr auto SLICE_ADMIN_CQ = SLICE_ADMIN_SQ + ADMIN_SQ_BYTES;
constexpr auto SLICE_IDENTIFY = SLICE_ADMIN_CQ + ADMIN_CQ_BYTES;
constexpr auto SLICE_IO_SQ = SLICE_IDENTIFY + IDENTIFY_BYTES;
constexpr auto SLICE_IO_CQ = SLICE_IO_SQ + IO_SQ_BYTES;

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

/// The 16-byte completion queue entry: the phase bit (bit 0 of word3)
/// flips each pass of the ring, and is how a polled driver tells a fresh
/// entry from a stale one.
struct CqEntry final {
    u32 command_specific = 0;
    u32 reserved = 0;
    u16 sq_head = 0;
    u16 sq_id = 0;
    u16 command_id = 0;
    u16 word3 = 0;
};
static_assert(sizeof(CqEntry) == 16);

/// MMIO reads and writes through the controller's mapping, volatile
/// because the device acts on every store.
class Regs final {
  public:
    explicit Regs(ours::VirtAddr base) : m_base(base) {}

    auto read64(usize const at) const -> u64 {
        return *reinterpret_cast<volatile u64 const *>(m_base + at);
    }
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

class AdminQueues final {
  public:
    AdminQueues(Regs const &regs, ours::VirtAddr sq, ours::VirtAddr cq, usize const stride)
        : m_regs(regs), m_sq(sq), m_cq(cq), m_stride(stride) {}

    /// Submits the command |entry| names (its CID is |id|), polls for the
    /// matching completion, and hands the entry back. False on timeout.
    /// The expected phase starts true: the host zeroes every entry's phase
    /// bit at creation, the controller writes 1 on the first pass, and the
    /// expected phase toggles each full pass of the ring -- so a fresh
    /// entry's bit matches the expected one, and a stale entry's differs.
    [[nodiscard]]
    auto submit(SqEntry const &entry, u16 const id, CqEntry &out) -> bool {
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
        m_tail = (m_tail + 1) % ADMIN_QUEUE_ENTRIES;
        // The doorbell takes the new tail, entries-as-count minus one
        // written as the value the specification names: the tail index the
        // device should consume up to... its field is the number of entries
        // minus the one already consumed; QEMU reads it as a count.
        m_regs.write32(NVME_SQ0TDBL, static_cast<u32>(m_tail));
        return wait_for(id, out);
    }

  private:
    /// Polls the completion queue for the entry whose command id is |id|,
    /// the phase bit naming it fresh. The head doorbell rings after.
    auto wait_for(u16 const id, CqEntry &out) -> bool {
        auto *const entries = reinterpret_cast<volatile CqEntry *>(m_cq);
        for (auto round = u32{0}; round < POLL_ROUNDS; ++round) {
            auto const &at = entries[m_head];
            auto const phase = (at.word3 & 1) != 0;
            if (phase != m_phase) {
                continue;
            }
            if (at.command_id == id) {
                out = CqEntry{at.command_specific, at.reserved, at.sq_head, at.sq_id,
                              at.command_id,       at.word3};
                m_head = (m_head + 1) % ADMIN_QUEUE_ENTRIES;
                if (m_head == 0) {
                    m_phase = !m_phase;
                }
                // The completion head doorbell: CQ0HDBL sits one stride
                // past SQ0TDBL.
                m_regs.write32(NVME_SQ0TDBL + m_stride, static_cast<u32>(m_head));
                return true;
            }
            ort::spin_hint();
        }
        return false;
    }

    Regs m_regs;
    ours::VirtAddr m_sq;
    ours::VirtAddr m_cq;
    u16 m_tail = 0;
    u16 m_head = 0;
    bool m_phase = true;
    usize m_stride = 0;
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

    auto read8(usize const at) const -> u8 {
        return *reinterpret_cast<volatile u8 const *>(m_at + at);
    }
    auto read16(usize const at) const -> u16 {
        return *reinterpret_cast<volatile u16 const *>(m_at + at);
    }
    auto read32(usize const at) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_at + at);
    }
    auto write32(usize const at, u32 const value) const -> void {
        *reinterpret_cast<volatile u32 *>(m_at + at) = value;
    }

  private:
    ours::VirtAddr m_at;
};

constinit std::array<loop::Slot, 40> SLOT_STORAGE{};

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

    // The namespace's size, from Identify Namespace over the admin queues:
    // the geometry the block service answers GetInfo with.
    auto admin = AdminQueues(regs, controller.admin_sq(), controller.admin_cq(), stride);
    auto cqe = CqEntry{};
    {
        auto entry = SqEntry{};
        entry.opcode = ADMIN_IDENTIFY;
        entry.nsid = 1;
        entry.prp1 = dma->phys() + SLICE_IDENTIFY;
        entry.cdw10 = CNS_IDENTIFY_NAMESPACE;
        if (!admin.submit(entry, 0, cqe)) {
            auto _ = ort::debug_line("nvme: Identify timed out");
            return 1;
        }
    }
    char nsze_text[10];
    auto const nsze = *reinterpret_cast<volatile u64 const *>(controller.identify());
    auto const nsze_blocks = static_cast<u32>(nsze);
    auto _ = ort::debug_line(
        {"nvme: controller ready, NSZE ", ort::decimal(nsze_blocks, nsze_text), " blocks"});
    if (nsze == 0) {
        auto _ = ort::debug_line("nvme: the namespace names no blocks");
        return 0;
    }

    // The outgoing directory, served when the block service lands on this
    // controller. For now the driver holds the machine and stops: nothing
    // exposes a protocol yet, and CC.EN is cleared before the destructors
    // run -- the pin discipline's order.
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"nvme: cannot make its port: ", ours::to_string(status)});
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    regs.write32(NVME_CC, 0);
    wait_silent([&] { return (regs.read32(NVME_CSTS) & CSTS_READY) == 0; }, 30, "CSTS.RDY 0");
    auto _ = ort::debug_line(
        {"nvme: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
