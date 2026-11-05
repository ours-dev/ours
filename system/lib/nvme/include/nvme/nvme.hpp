/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef NVME_NVME_HPP
#define NVME_NVME_HPP 1

#include <array>
#include <span>

#include <cstdint>
#include <cstring>

#include <ort/spin_hint.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <pci/bar.hpp>
#include <pci/pci.hpp>
#include <ustl/result.hpp>

/// The NVMe controller's device core: the register file's layout, the queue
/// pairs, and the bring-up sequence, over an MMIO mapping and a DMA region
/// the caller owns (the driver maps BAR0 through the MmIo root and allocates
/// the region from vmo_create_contiguous; a host test maps neither, and
/// points the same code at plain memory). What is here is the part a
/// controller datasheet fixes -- which register holds what, what a bring-up
/// writes in which order, how a polled completion names itself -- so it
/// builds in every toolchain, the host one included, where its tests run a
/// fake controller over an array.
///
/// What is not here: kernel objects and the wire. No BTI, no VMO, no
/// publish -- the driver assembles those around this core, and its Control
/// protocol is a protocols family of its own.
namespace nvme {

/// The register file's layout, at byte offsets from BAR0.
inline constexpr auto NVME_CAP = usize{0x00};
inline constexpr auto NVME_VS = usize{0x08};
inline constexpr auto NVME_CC = usize{0x14};
inline constexpr auto NVME_CSTS = usize{0x1C};
inline constexpr auto NVME_AQA = usize{0x24};
inline constexpr auto NVME_ASQ = usize{0x28};
inline constexpr auto NVME_ACQ = usize{0x30};
/// The doorbell array's base: queue pair |qid| rings its SQ tail at
/// NVME_SQ0TDBL + 2*qid*stride and its CQ head one stride further, where
/// stride is CAP.DSTRD's (1 << (2 + DSTRD)); the admin pair is qid 0.
inline constexpr auto NVME_SQ0TDBL = usize{0x1000};

/// The mapping a BAR0 needs: 16 KiB of register file, the doorbells at
/// 0x1000 and above included.
inline constexpr auto REGS_BYTES = usize{16 * 4096};

/// CC's bits: enabled, and the I/O queue entry sizes in log2 units (64-byte
/// SQ entries = 6, 16-byte CQE entries = 4), written before EN goes high.
inline constexpr u64 CC_ENABLE = 1;
inline constexpr auto CC_IOSQES_SHIFT = usize{16};
inline constexpr auto CC_IOCQES_SHIFT = usize{20};
inline constexpr u64 CC_IOSQES_VALUE = u64{6} << CC_IOSQES_SHIFT;
inline constexpr u64 CC_IOCQES_VALUE = u64{4} << CC_IOCQES_SHIFT;

/// CSTS's bits: ready, and the fatal status.
inline constexpr u64 CSTS_READY = 1;
inline constexpr u64 CSTS_FATAL = u64{1} << 1;

/// The admin queue depths: 2 entries each carries bring-up's commands, run
/// one at a time. AQA packs both as (count - 1).
inline constexpr auto ADMIN_QUEUE_ENTRIES = usize{2};
/// The I/O queue pair's depth: one slot is in flight at a time, the block
/// service's tag depth, and the queue carries it with room to spare.
inline constexpr auto IO_QUEUE_ENTRIES = usize{4};
inline constexpr u32 AQA_ASQS_FIELD = u32{ADMIN_QUEUE_ENTRIES - 1};
inline constexpr u32 AQA_ACQS_FIELD = u32{ADMIN_QUEUE_ENTRIES - 1} << 16;

/// The admin command opcodes: Identify Namespace (CNS 0 names a namespace),
/// and the I/O queue pair's creation, the CQ first, for the SQ to name it.
inline constexpr u8 ADMIN_CREATE_IO_SQ = 0x01;
inline constexpr u8 ADMIN_CREATE_IO_CQ = 0x05;
inline constexpr u8 ADMIN_IDENTIFY = 6;
inline constexpr u32 CNS_IDENTIFY_NAMESPACE = 0;
/// The I/O command opcodes, NVM command set.
inline constexpr u8 IO_WRITE = 0x01;
inline constexpr u8 IO_READ = 0x02;
/// The one namespace this driver claims, and the I/O queues' id: 0 is the
/// admin pair's.
inline constexpr u32 NSID = 1;
inline constexpr u16 QID_IO = 1;

/// A mass-storage controller whose specific subclass is the NVM one: class
/// base 01h, subclass 08h names every vendor's NVMe. The class code as the
/// config space packs it: base<<16 | sub<<8 | iface.
inline constexpr u32 CLASS_NVM = (u32{0x01} << 16) | (u32{0x08} << 8);

/// DMA layout: every slice page-aligned, because PRP wants pages, and the
/// slices' offsets are the constant map of the region. One data page per
/// in-flight request: the largest read, 7 blocks, fits a page, so PRP1 alone
/// names the buffer and PRP2 stays zero.
inline constexpr auto PAGE = usize{4096};
inline constexpr auto ADMIN_SQ_BYTES = 2 * PAGE;
inline constexpr auto ADMIN_CQ_BYTES = 2 * PAGE;
inline constexpr auto IDENTIFY_BYTES = PAGE;
inline constexpr auto IO_SQ_BYTES = 2 * PAGE;
inline constexpr auto IO_CQ_BYTES = 2 * PAGE;
inline constexpr auto DATA_BYTES = PAGE;
inline constexpr auto DMA_BYTES =
    ADMIN_SQ_BYTES + ADMIN_CQ_BYTES + IDENTIFY_BYTES + IO_SQ_BYTES + IO_CQ_BYTES + DATA_BYTES;
inline constexpr auto SLICE_ADMIN_SQ = usize{0};
inline constexpr auto SLICE_ADMIN_CQ = SLICE_ADMIN_SQ + ADMIN_SQ_BYTES;
inline constexpr auto SLICE_IDENTIFY = SLICE_ADMIN_CQ + ADMIN_CQ_BYTES;
inline constexpr auto SLICE_IO_SQ = SLICE_IDENTIFY + IDENTIFY_BYTES;
inline constexpr auto SLICE_IO_CQ = SLICE_IO_SQ + IO_SQ_BYTES;
inline constexpr auto SLICE_DATA = SLICE_IO_CQ + IO_CQ_BYTES;

/// How long a controller may take to leave or reach CSTS.RDY, in seconds:
/// CAP.TO bounds it in 500 ms units -- 7.5 s on QEMU -- and the wait allows
/// four times that.
inline constexpr u64 READY_SECONDS = 30;

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

/// The 16-byte completion queue entry, as the device lays it out. The third
/// dword names the submission queue's consumed head and its id; the fourth
/// carries the command id in its low half, the phase at bit 16 -- the bit a
/// polled driver tells a fresh entry from a stale one by, which flips each
/// pass of the ring -- and the status field above, whose clear value names
/// success.
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
constexpr auto is_command_ok(u32 const dw3) -> bool {
    return (dw3 & 0xFFFE'0000u) == 0;
}

/// MMIO reads and writes through the controller's mapping, volatile because
/// the device acts on every store. On the host the mapping is plain memory,
/// and the same loads and stores run against a test's model of the
/// controller.
///
/// The model hangs off Regs as a Sink: a store that acts on the device --
/// CC.EN raising the ready bit, a doorbell ringing a queue -- acts
/// synchronously with the store, as it does on silicon. Nothing on target
/// sets one; a test's fake controller does. The 64-bit stores (the admin
/// queues' bases) need no sink: the controller latches them when it is
/// enabled, which a sink's own handler reads from the memory it models.
class Regs final {
  public:
    /// The device side of the register file, what a model of the controller
    /// supplies.
    class Sink {
      public:
        /// The 32-bit store of |value| at |at| has landed.
        virtual auto on_write32(usize at, u32 value) -> void = 0;

      protected:
        Sink() = default;
        Sink(Sink const &) = default;
        auto operator=(Sink const &) -> Sink & = default;
        ~Sink() = default;
    };

    Regs(ours::VirtAddr const base, Sink *const sink = nullptr) : m_base(base), m_sink(sink) {}

    [[nodiscard]]
    auto read32(usize const at) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_base + at);
    }
    auto write64(usize const at, u64 const value) const -> void {
        *reinterpret_cast<volatile u64 *>(m_base + at) = value;
    }
    auto write32(usize const at, u32 const value) const -> void {
        *reinterpret_cast<volatile u32 *>(m_base + at) = value;
        if (m_sink != nullptr) {
            m_sink->on_write32(at, value);
        }
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
    Sink *m_sink = nullptr;
};

/// The DMA region's constant slice map: what each slice's offset is, and
/// the region's two ends as the addresses the controller and the host see.
/// The caller owns the region (vmo_create_contiguous on target, a buffer on
/// host); this maps it.
class DmaMap final {
  public:
    DmaMap(ours::PhysAddr phys, ours::VirtAddr virt) : m_phys(phys), m_virt(virt) {}

    [[nodiscard]]
    auto admin_sq_phys() const -> ours::PhysAddr {
        return m_phys + SLICE_ADMIN_SQ;
    }
    [[nodiscard]]
    auto admin_cq_phys() const -> ours::PhysAddr {
        return m_phys + SLICE_ADMIN_CQ;
    }
    /// The Identify page, the geometry command's target.
    [[nodiscard]]
    auto identify_phys() const -> ours::PhysAddr {
        return m_phys + SLICE_IDENTIFY;
    }
    [[nodiscard]]
    auto admin_sq() const -> ours::VirtAddr {
        return m_virt + SLICE_ADMIN_SQ;
    }
    [[nodiscard]]
    auto admin_cq() const -> ours::VirtAddr {
        return m_virt + SLICE_ADMIN_CQ;
    }
    [[nodiscard]]
    auto identify() const -> ours::VirtAddr {
        return m_virt + SLICE_IDENTIFY;
    }
    /// The in-flight request's buffer: the one data page, aligned as PRP
    /// wants, where a Read's blocks land and a Write's start from. One page
    /// carries the largest read -- 7 blocks -- and the tag depth is 1, so
    /// the request in flight always owns it.
    [[nodiscard]]
    auto data() const -> ours::VirtAddr {
        return m_virt + SLICE_DATA;
    }
    [[nodiscard]]
    auto data_phys() const -> ours::PhysAddr {
        return m_phys + SLICE_DATA;
    }
    [[nodiscard]]
    auto io_sq_phys() const -> ours::PhysAddr {
        return m_phys + SLICE_IO_SQ;
    }
    [[nodiscard]]
    auto io_cq_phys() const -> ours::PhysAddr {
        return m_phys + SLICE_IO_CQ;
    }
    [[nodiscard]]
    auto io_sq() const -> ours::VirtAddr {
        return m_virt + SLICE_IO_SQ;
    }
    [[nodiscard]]
    auto io_cq() const -> ours::VirtAddr {
        return m_virt + SLICE_IO_CQ;
    }

  private:
    ours::PhysAddr m_phys;
    ours::VirtAddr m_virt;
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

/// Spins |until| until true or |seconds| pass, measured by |clock| (the
/// monotonic clock in microseconds). |each_second| is called with the whole
/// seconds as they tick by -- the driver's keepalive line hangs here, so a
/// silent console's watchdog is fed on target and the tests' fake clocks
/// are fed nothing. The wait is wall-clock because the controller's ready
/// transitions run on the device's own timers (CAP.TO, 500 ms units), which
/// advance at the machine's pace, not the driver's.
template <typename Until, typename Clock, typename EachSecond>
auto wait_until(Until &&until, u64 const seconds, Clock &&clock, EachSecond &&each_second) -> bool {
    auto const started = clock();
    auto printed = u64{0};
    for (;;) {
        if (until()) {
            return true;
        }
        auto const elapsed = static_cast<u64>((clock() - started) / 1'000'000);
        if (elapsed > printed) {
            printed = elapsed;
            if (elapsed >= seconds) {
                return false;
            }
            each_second(elapsed);
        }
        ort::spin_hint();
    }
}

/// The address range a probe assigns new BARs from: hands runs upward from
/// its base, each aligned to its size. This round's arena begins at the
/// ECAM window's end -- the route service names the window, and the decode
/// space behind it is unassigned on the machines this runs on; a firmware
/// that reserves it would hand the arena a hole, which is a repair item,
/// not a silent overlap.
class Arena final {
  public:
    Arena(ours::PhysAddr const base, ours::PhysAddr const limit) : m_next(base), m_limit(limit) {}

    /// The next |size| run, aligned up to |size|'s boundary (a power of
    /// two, as a BAR's decode is). NoResource when the run would cross the
    /// arena's limit.
    [[nodiscard]]
    auto take(u64 const size) -> ustl::Result<ours::PhysAddr, ours::Status> {
        auto const at = ours::PhysAddr{(std::to_underlying(m_next) + (size - 1)) & ~(size - 1)};
        if (size == 0 || (std::to_underlying(at) + size) > std::to_underlying(m_limit)) {
            return ustl::err(ours::Status::NoResource);
        }
        m_next = at + static_cast<usize>(size);
        return ustl::ok(at);
    }

  private:
    ours::PhysAddr m_next;
    ours::PhysAddr m_limit;
};

/// One function the probe claimed: where it sits on the bus, and the base
/// its register file decodes at -- kept from the firmware's assignment, or
/// freshly assigned from the arena.
struct Found final {
    usize index = 0;
    ours::PhysAddr bar0{};
};

/// Walks bus 0 for the first mass-storage/NVM function (class 01h/08h, the
/// class every vendor's NVMe names) whose BAR0 is a 64-bit memory BAR and
/// that the |owned| indices do not already name -- the driver's running
/// functions, which a rescan must skip. A function whose BAR0 carries no
/// base (a hot-plugged one: firmware assigns BARs at boot only) is sized,
/// assigned from |arena| and enabled; one with a base keeps it. NotFound
/// when the walk sees none of that shape.
[[nodiscard]]
auto probe(pci::Config &config, Arena &arena, std::span<usize const> owned)
    -> ustl::Result<Found, ours::Status>;

/// The controller as the driver holds it: the register file, the DMA
/// slices, and the two queue pairs the bring-up and the block service
/// drive. One controller, one namespace, one I/O queue pair; commands run
/// one at a time.
class Device final {
  public:
    /// Over |bar0|, the register file's mapping, and the DMA region the
    /// caller allocated, sliced by DmaMap's constants. |sink|, the device
    /// side a model supplies, is the tests'; null on target. The doorbell
    /// stride is CAP's own, read here: CAP answers while the controller is
    /// disabled, and the doorbells' offsets are stride arithmetic.
    Device(ours::VirtAddr const bar0, ours::PhysAddr const dma_phys, ours::VirtAddr const dma_virt,
           Regs::Sink *const sink = nullptr)
        : m_regs(bar0, sink)
        , m_dma(dma_phys, dma_virt)
        , m_stride(m_regs.doorbell_stride())
        , m_admin(m_regs, m_dma.admin_sq(), m_dma.admin_cq(), NVME_SQ0TDBL, NVME_SQ0TDBL + m_stride,
                  ADMIN_QUEUE_ENTRIES)
        , m_io(m_regs, m_dma.io_sq(), m_dma.io_cq(), NVME_SQ0TDBL + 2 * QID_IO * m_stride,
               NVME_SQ0TDBL + (2 * QID_IO + 1) * m_stride, IO_QUEUE_ENTRIES) {}

    /// Bring-up, in the order the specification fixes: disable, wait the
    /// ready bit to fall; the admin queues' count and bases; the I/O queue
    /// entry sizes, then enable; wait the ready bit to rise; and the fatal
    /// bit checked last. |clock| is the monotonic clock in microseconds and
    /// |each_second| the keepalive called with each whole second of the
    /// waits. TimeOut when a wait ran out; BadState on the fatal bit.
    template <typename Clock, typename EachSecond>
    auto bring_up(Clock &&clock, EachSecond &&each_second) -> ours::Status {
        m_regs.write32(NVME_CC, 0);
        if (!wait_until([&] { return (m_regs.read32(NVME_CSTS) & CSTS_READY) == 0; }, READY_SECONDS,
                        clock, each_second)) {
            return ours::Status::TimeOut;
        }
        // The admin queues: base addresses must be page-aligned at least,
        // and the region's slices are page-aligned.
        m_regs.write32(NVME_AQA, AQA_ACQS_FIELD | AQA_ASQS_FIELD);
        m_regs.write64(NVME_ASQ, std::to_underlying(m_dma.admin_sq_phys()));
        m_regs.write64(NVME_ACQ, std::to_underlying(m_dma.admin_cq_phys()));
        // The I/O queue entry sizes, then enable.
        auto const cc_sizes = static_cast<u32>(CC_IOSQES_VALUE | CC_IOCQES_VALUE);
        m_regs.write32(NVME_CC, cc_sizes);
        m_regs.write32(NVME_CC, cc_sizes | static_cast<u32>(CC_ENABLE));
        if (!wait_until([&] { return (m_regs.read32(NVME_CSTS) & CSTS_READY) != 0; }, READY_SECONDS,
                        clock, each_second)) {
            return ours::Status::TimeOut;
        }
        if ((m_regs.read32(NVME_CSTS) & CSTS_FATAL) != 0) {
            return ours::Status::BadState;
        }
        return ours::Status::Ok;
    }

    /// The namespace's block count, from Identify Namespace over the admin
    /// queues -- the geometry the block service answers GetInfo with.
    /// TimeOut when the controller never answered; BadState when its
    /// completion carries a status, which says itself on the console (the
    /// driver's line, not here's).
    [[nodiscard]]
    auto identify_blocks() -> ustl::Result<u64, ours::Status> {
        auto entry = SqEntry{};
        entry.opcode = ADMIN_IDENTIFY;
        entry.nsid = NSID;
        entry.prp1 = std::to_underlying(m_dma.identify_phys());
        entry.cdw10 = CNS_IDENTIFY_NAMESPACE;
        auto cqe = CqEntry{};
        if (!m_admin.submit(entry, 0, cqe)) {
            return ustl::err(ours::Status::TimeOut);
        }
        if (!is_command_ok(cqe.word3)) {
            return ustl::err(ours::Status::BadState);
        }
        auto const nsze = *reinterpret_cast<volatile u64 const *>(m_dma.identify());
        return ustl::ok(nsze);
    }

    /// The I/O queue pair, created over the admin queues -- the CQ first,
    /// for the SQ to name it. Queue size is the entry count itself (AQA's
    /// counts were minus one; these are not); the CQ is physically
    /// contiguous, its interrupts not enabled -- the poll model -- and the
    /// SQ names this CQ at priority zero, which polling ignores.
    auto create_io_queues() -> ours::Status {
        auto cqe = CqEntry{};
        auto entry = SqEntry{};
        entry.opcode = ADMIN_CREATE_IO_CQ;
        entry.prp1 = std::to_underlying(m_dma.io_cq_phys());
        entry.cdw10 = static_cast<u32>(IO_QUEUE_ENTRIES) << 16 | QID_IO;
        entry.cdw11 = 0x1 /* physically contiguous */;
        if (!m_admin.submit(entry, 1, cqe)) {
            return ours::Status::TimeOut;
        }
        if (!is_command_ok(cqe.word3)) {
            return ours::Status::BadState;
        }
        entry = SqEntry{};
        entry.opcode = ADMIN_CREATE_IO_SQ;
        entry.prp1 = std::to_underlying(m_dma.io_sq_phys());
        entry.cdw10 = static_cast<u32>(IO_QUEUE_ENTRIES) << 16 | QID_IO;
        entry.cdw11 = 0x1 /* physically contiguous */ | u32{QID_IO} << 16;
        if (!m_admin.submit(entry, 2, cqe)) {
            return ours::Status::TimeOut;
        }
        if (!is_command_ok(cqe.word3)) {
            return ours::Status::BadState;
        }
        return ours::Status::Ok;
    }

    /// The views the block service reads: the I/O pair to submit through,
    /// and the data page the commands' PRP1 names.
    [[nodiscard]]
    auto io() -> Queues & {
        return m_io;
    }
    [[nodiscard]]
    auto data() const -> ours::VirtAddr {
        return m_dma.data();
    }
    [[nodiscard]]
    auto data_phys() const -> ours::PhysAddr {
        return m_dma.data_phys();
    }

  private:
    Regs m_regs;
    DmaMap m_dma;
    usize m_stride = 0;
    Queues m_admin;
    Queues m_io;
};
} // namespace nvme

#endif // #ifndef NVME_NVME_HPP
