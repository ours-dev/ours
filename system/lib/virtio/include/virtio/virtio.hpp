/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef VIRTIO_VIRTIO_HPP
#define VIRTIO_VIRTIO_HPP 1

#include <array>
#include <span>

#include <cstring>

#include <ort/spin_hint.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <pci/bar.hpp>
#include <pci/pci.hpp>
#include <ustl/result.hpp>

/// The virtio legacy (transitional) device's core: the register access
/// interface, the legacy ring's layout, the bring-up sequence, the submit
/// path, and the probe -- over port I/O and a DMA region the caller
/// supplies. What is here is what the legacy specification fixes: which
/// port holds which register, what a bring-up writes in which order, how a
/// descriptor chain is offered and how its completion names itself -- so it
/// builds in every toolchain, the host one included, where its tests run a
/// fake device over an array.
///
/// Port I/O is not memory: an in or out is an instruction, and a host test
/// runs none. The registers are reached through the Ports interface, whose
/// target implementation is the driver's x86 in and out (the ioports
/// resource has opened the range to the process), and whose test one is a
/// memory-backed fake -- the same seam the nvme core cuts with its Sink,
/// on the access side rather than the reaction side.
///
/// What is not here: kernel objects and the wire. No ioports_request, no
/// BTI, no interrupt object, no publish -- the driver assembles those
/// around this core, and its Control protocol is a protocols family of its
/// own.
namespace virtio {

/// The device status bits, in the order the specification demands them.
inline constexpr u8 STATUS_ACKNOWLEDGE = 1;
inline constexpr u8 STATUS_DRIVER = 2;
inline constexpr u8 STATUS_DRIVER_OK = 4;
inline constexpr u8 STATUS_FEATURES_OK = 8;

/// A descriptor's flags.
inline constexpr u16 DESC_NEXT = 1;
inline constexpr u16 DESC_WRITE = 2;

/// The virtio vendor, and the block device's id in both forms.
inline constexpr u16 VIRTIO_VENDOR = 0x1AF4;
inline constexpr u16 VIRTIO_BLK_TRANSITIONAL = 0x1001;
inline constexpr u16 VIRTIO_BLK_MODERN = 0x1042;

/// One page, the unit a legacy ring's page frame number names; one sector,
/// plus the request header that precedes the data and the status byte that
/// follows it.
inline constexpr auto PAGE = usize{4096};
inline constexpr auto SECTOR = usize{512};
inline constexpr auto HEADER = usize{16};

/// The queue's depth, the driver's choice within the device's maximum. A
/// legacy ring is the descriptor table, the available ring after it, and
/// the used ring on the next page boundary, so it spans two pages whatever
/// the depth: the used ring always starts on the page after the one the
/// page frame number names.
inline constexpr auto LEGACY_QUEUE_NUM = usize{256};
inline constexpr auto DESC_BYTES = LEGACY_QUEUE_NUM * 16;
inline constexpr auto AVAIL_OFFSET = DESC_BYTES;
inline constexpr auto AVAIL_BYTES = 6 + LEGACY_QUEUE_NUM * 2;
inline constexpr auto USED_OFFSET = (AVAIL_OFFSET + AVAIL_BYTES + PAGE - 1) & ~(PAGE - 1);
inline constexpr auto USED_END = USED_OFFSET + 6 + LEGACY_QUEUE_NUM * 8;

/// The contiguous run: the ring's pages, then the request behind the used
/// ring, on a 16-byte boundary. Its tail holds the biggest request the
/// protocol carries: header, MAX_READ_SECTORS blocks, status.
inline constexpr auto REGION_PAGES = usize{4};
inline constexpr auto REGION_BYTES = REGION_PAGES * PAGE;
inline constexpr auto REQUEST_OFFSET = (USED_END + 15) & ~usize{15};
inline constexpr auto DATA_OFFSET = REQUEST_OFFSET + HEADER;

/// The register offsets, in bytes from the device's I/O base: one field
/// each, at one offset, with one width -- the shape the modern common
/// config lacks on this QEMU build.
inline constexpr u16 REG_HOST_FEATURES = 0x00;
inline constexpr u16 REG_GUEST_FEATURES = 0x04;
inline constexpr u16 REG_QUEUE_PFN = 0x08;
inline constexpr u16 REG_QUEUE_NUM = 0x0C;
inline constexpr u16 REG_QUEUE_SEL = 0x0E;
inline constexpr u16 REG_QUEUE_NOTIFY = 0x10;
inline constexpr u16 REG_STATUS = 0x12;
inline constexpr u16 REG_ISR = 0x13;
inline constexpr u16 REG_DEVICE_CONFIG = 0x14;

/// The one device path's bound: how many parks (each one an interrupt
/// wait on target, an immediate return on the host) a submit makes before
/// it names TimeOut. The device answers on its own pace; a hundred lines
/// is a hundred of its completions, and an un answered one is a device
/// gone wrong.
inline constexpr u32 SUBMIT_PARKS = 100;

/// Port I/O, as the legacy registers need it. The target implementation is
/// the x86 in and out, which ioports_request has opened to the process's
/// threads; a test's is a memory-backed fake that can act on the stores.
/// One interface, no default: a caller that reaches the registers without
/// an implementation does not link.
class Ports {
  public:
    virtual auto in8(u16 offset) const -> u8 = 0;
    virtual auto in16(u16 offset) const -> u16 = 0;
    virtual auto in32(u16 offset) const -> u32 = 0;
    virtual auto out8(u16 offset, u8 value) const -> void = 0;
    virtual auto out16(u16 offset, u16 value) const -> void = 0;
    virtual auto out32(u16 offset, u32 value) const -> void = 0;

  protected:
    Ports() = default;
    Ports(Ports const &) = default;
    auto operator=(Ports const &) -> Ports & = default;
    ~Ports() = default;
};

/// The legacy registers, over |ports|: one accessor per register, the
/// offsets and widths above. Nothing here decides -- each function is one
/// access the specification names.
class LegacyRegs final {
  public:
    explicit LegacyRegs(Ports const &ports) : m_ports(ports) {}

    /// HOST_FEATURES, and the features the driver accepts: a legacy driver
    /// needs none of the device's.
    [[nodiscard]]
    auto host_features() const -> u32 {
        return m_ports.in32(REG_HOST_FEATURES);
    }

    auto accept_no_features() const -> void {
        m_ports.out32(REG_GUEST_FEATURES, 0);
    }

    /// QUEUE_PFN: the ring's physical address, named by page frame.
    auto set_queue_pfn(ours::PhysAddr const paddr) const -> void {
        m_ports.out32(REG_QUEUE_PFN, static_cast<u32>(std::to_underlying(paddr) >> 12));
    }

    /// QUEUE_NUM: the depth the device runs this queue at, read as its
    /// maximum and written as the driver's choice.
    [[nodiscard]]
    auto queue_num() const -> u16 {
        return m_ports.in16(REG_QUEUE_NUM);
    }

    auto set_queue_num(u16 const num) const -> void {
        m_ports.out16(REG_QUEUE_NUM, num);
    }

    auto select_queue(u16 const index) const -> void {
        m_ports.out16(REG_QUEUE_SEL, index);
    }

    /// QUEUE_NOTIFY: rung with the queue's index.
    auto notify(u16 const index) const -> void {
        m_ports.out16(REG_QUEUE_NOTIFY, index);
    }

    [[nodiscard]]
    auto status() const -> u8 {
        return m_ports.in8(REG_STATUS);
    }

    auto set_status(u8 const value) const -> void {
        m_ports.out8(REG_STATUS, value);
    }

    /// ISR status: which of the ring and the config asked for attention.
    /// One read answers and clears it, which drops the device's interrupt
    /// line -- what the driver does after each wait, so the level the
    /// device holds does not re-fire the moment the kernel unmasks.
    [[nodiscard]]
    auto isr() const -> u8 {
        return m_ports.in8(REG_ISR);
    }

    /// The device config's capacity, in blocks: two reads of the u64 the
    /// legacy layout puts behind the common registers, without MSI-X
    /// enabled.
    [[nodiscard]]
    auto capacity() const -> u64 {
        return static_cast<u64>(m_ports.in32(REG_DEVICE_CONFIG)) |
               (static_cast<u64>(m_ports.in32(REG_DEVICE_CONFIG + 4)) << 32);
    }

  private:
    Ports const &m_ports;
};

/// The port-space range a probe assigns new BARs from: hands runs upward
/// from its base, each aligned to its size. This round's arena begins at
/// 0xE000 -- the tail of the 16-bit port space, past the ranges this
/// machine's chipset and its firmware-assigned devices sit in; a machine
/// that assigns there is a repair item, not a silent overlap.
class IoArena final {
  public:
    IoArena(u16 const base, u16 const limit) : m_next(base), m_limit(limit) {}

    /// The next |size|-port run, aligned to |size|'s boundary. NoResource
    /// when the run would cross the arena's limit -- the alignment is
    /// computed wide, since a base under the limit can still round up past
    /// 16 bits.
    [[nodiscard]]
    auto take(u32 const size) -> ustl::Result<u16, ours::Status> {
        auto const at = (static_cast<u32>(m_next) + (size - 1)) & ~(size - 1);
        if (size == 0 || at > 0xFFFF || at + size > m_limit) {
            return ustl::err(ours::Status::NoResource);
        }
        m_next = static_cast<u16>(at + size);
        return ustl::ok(static_cast<u16>(at));
    }

  private:
    u16 m_next;
    u32 m_limit;
};

/// One function the probe claimed: where it sits on the bus -- the root
/// bus slot the chipset's interrupt wiring asks for -- and the I/O base
/// its legacy registers live at, kept from the firmware's assignment or
/// freshly assigned from the arena.
struct Found final {
    usize index = 0;
    u16 io_base = 0;
};

/// Walks bus 0 for the first virtio block function -- vendor 0x1AF4, device
/// id 0x1001 (transitional) or 0x1042 (modern-only, which the legacy
/// interface drives the same) -- whose BAR0 is an I/O BAR, and that the
/// |owned| indices do not already name. A function whose BAR0 carries no
/// base (a hot-plugged one: firmware assigns BARs at boot only) is sized,
/// assigned from |arena| and enabled. NotFound when the walk sees none of
/// that shape.
[[nodiscard]]
auto probe(pci::Config &config, IoArena &arena, std::span<usize const> owned)
    -> ustl::Result<Found, ours::Status>;

/// The device, once brought up: everything the ring and the requests
/// touch. |region_map| and |region_phys| are the caller's DMA region (the
/// pin and the mapping are the driver's), and the legacy registers are
/// reached through |ports|.
class Device final {
  public:
    Device(Ports const &ports, ours::VirtAddr const region_map, ours::PhysAddr const region_phys)
        : m_regs(ports), m_region_map(region_map), m_region_phys(region_phys) {}

    /// The device's capacity, in blocks, for GetInfo.
    [[nodiscard]]
    auto blocks() const -> u64 {
        return m_regs.capacity();
    }

    /// Bring-up, in the order the legacy specification fixes: reset and
    /// observed reset; acknowledge; driver; the device's features read and
    /// none accepted; queue 0 selected, its maximum read and the driver's
    /// depth written; the ring's page named; and DRIVER_OK last. The ring
    /// lives at |region_phys|, in the layout the constants above lay out.
    /// TimeOut when the reset never took.
    auto bring_up(ours::PhysAddr const region_phys) -> ours::Status {
        m_regs.set_status(0);
        // The device answers a reset on its own pace; a million reads of
        // the status register is far past any device's answer and far
        // under a second of spins.
        for (auto tries = u32{0}; tries < 1'000'000; ++tries) {
            if (m_regs.status() == 0) {
                break;
            }
            ort::spin_hint();
        }
        if (m_regs.status() != 0) {
            return ours::Status::TimeOut;
        }
        m_regs.set_status(STATUS_ACKNOWLEDGE);
        m_regs.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER);
        (void)m_regs.host_features();
        m_regs.accept_no_features();
        m_regs.select_queue(0);
        auto const queue_max = m_regs.queue_num();
        if (queue_max == 0) {
            return ours::Status::BadState;
        }
        // The legacy depth is the driver's to choose, within the device's
        // maximum: written back before the ring is registered, so the
        // device computes the rings' addresses from the same depth this
        // core laid them out with.
        m_regs.set_queue_num(static_cast<u16>(LEGACY_QUEUE_NUM));
        m_regs.set_queue_pfn(region_phys);
        m_regs.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK |
                          STATUS_DRIVER_OK);
        return ours::Status::Ok;
    }

    /// Reads |count| blocks starting at |lba| into the region's data area,
    /// and returns the region offset the data landed at -- the caller reads
    /// it there, through the same mapping. |park| is called whenever the
    /// ring holds no answer yet: on target it parks the thread on the
    /// device's interrupt line (and answers the line, one read of the ISR
    /// register, so the level the device holds does not re-fire); the
    /// whole request runs on the caller's thread, and requests serialize
    /// on it, which one device queue makes true anyway.
    template <typename Park>
    auto read_blocks(u64 const lba, usize const count, Park &&park) -> ours::Status {
        return submit(lba, count, false, park);
    }

    /// Writes |count| blocks starting at |lba| from the region's data
    /// area, which the caller filled before the call.
    template <typename Park>
    auto write_blocks(u64 const lba, usize const count, Park &&park) -> ours::Status {
        return submit(lba, count, true, park);
    }

  private:
    /// The one device path, both directions: |write| flips the header's
    /// type and the data descriptor's direction -- the device reads a
    /// write's data and writes a read's. The wait checks the used ring
    /// first, then parks -- an answer left by the last request is taken
    /// without a park at all.
    template <typename Park>
    auto submit(u64 const lba, usize const count, bool const write, Park &&park) -> ours::Status {
        if (count == 0 || DATA_OFFSET + count * SECTOR + 1 > REGION_BYTES) {
            return ours::Status::InvArgs;
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
        put64(0, 0, std::to_underlying(buf_base));
        put32(0, 8, HEADER);
        put16(0, 12, DESC_NEXT);
        put16(0, 14, 1);
        // desc[1]: the blocks, chained to desc[2]. A write's data flows
        // toward the device, so the WRITE flag -- what the device writes --
        // is on only for a read.
        put64(1, 0, std::to_underlying(buf_base + HEADER));
        put32(1, 8, static_cast<u32>(count * SECTOR));
        put16(1, 12, DESC_NEXT | (write ? u16{0} : DESC_WRITE));
        put16(1, 14, 2);
        // desc[2]: the status byte, written by the device, end of the
        // chain. It sits right after the data, which starts HEADER into
        // the buffer: drop the HEADER and the device writes the status one
        // header's width into the data's tail, and the byte read back at
        // the true status slot is whatever the region held before.
        put64(2, 0, std::to_underlying(buf_base + HEADER + count * SECTOR));
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

        // The device raises the line when the chain's entry lands in the
        // used ring; the park waits for it. A level line stays masked from
        // its delivery until its ack, and each park is also the ack of the
        // one before it, so the park that comes back from request N is
        // what arms the line for request N + 1. Reading the ISR register
        // after each park clears the device's own level, without which the
        // unmasked line would re-fire before any new completion. A park
        // can come back without this request's entry -- an interrupt the
        // last request left pending -- so the entry is what ends the loop,
        // not the park.
        auto *const used = bytes + USED_OFFSET;
        for (auto waits = u32{0}; waits < SUBMIT_PARKS; ++waits) {
            if (*reinterpret_cast<volatile u16 const *>(used + 2) == static_cast<u16>(m_seen + 1)) {
                m_seen += 1;
                auto const status_byte = *reinterpret_cast<volatile u8 const *>(
                    bytes + REQUEST_OFFSET + HEADER + count * SECTOR);
                return status_byte == 0 ? ours::Status::Ok : ours::Status::BadState;
            }
            if (auto const parked = park(); parked != ours::Status::Ok) {
                return parked;
            }
            (void)m_regs.isr();
        }
        return ours::Status::TimeOut;
    }

    LegacyRegs m_regs;
    ours::VirtAddr m_region_map;
    ours::PhysAddr m_region_phys;
    /// The chains offered so far, and the used entries seen so far: the
    /// two cursors of the ring, the device's behind the driver's by
    /// exactly the requests in flight.
    u64 m_offered = 0;
    u64 m_seen = 0;
};
} // namespace virtio

#endif // #ifndef VIRTIO_VIRTIO_HPP
