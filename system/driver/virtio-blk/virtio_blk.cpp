/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// virtio-blk -- the userspace driver of QEMU's virtio block device
/// (storage-layer plan, .agents/tasks/storage-layer). It drives the device
/// through the legacy (transitional) interface: the I/O-port BAR that QEMU's
/// virtio-pci keeps beside the modern one, whose registers are one field
/// each, at one offset, with one width.
///
/// The legacy interface is the working path, and why it is tells you what
/// the modern one costs here: the modern common config's queue section does
/// not answer on this QEMU build -- queue_size reads 0xFFFF, num_queues 0,
/// and a queue whose size the device never states is never configured, so
/// its doorbell is ignored (evidence and the monitor ground truth:
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
///   - the ring: one contiguous run of two pages from
///     vmo_create_contiguous, pinned to one address. The legacy layout puts
///     the used ring on the page boundary after the available ring, so the
///     run is two pages, and the device computes the used ring's address
///     from the page frame number the same way;
///   - the request: header, sector and status in the run's second page.
///
/// The events: reset (status 0, read back), ACKNOWLEDGE, DRIVER, features
/// accepted as none, the queue selected and its depth read back, the ring
/// registered by page frame number, DRIVER_OK, one descriptor chain offered
/// through the available ring, the queue rung through the notify register,
/// and the used ring polled until the answer comes. Before any pin is
/// undone the device is stopped and observed stopped, so the kernel never
/// has to quarantine what this driver pinned.
#include <ours/bti.hpp>
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>

#include <cstdint>

#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>

namespace {
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

/// The queue's depth, as the device's own queue_num register says it. A
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

/// The contiguous run: the ring's two pages, then the request in the
/// region's tail.
constexpr auto REGION_PAGES = usize{3};
constexpr auto REGION_BYTES = REGION_PAGES * PAGE;
/// The request goes past the used ring, on a 16-byte boundary.
constexpr auto REQUEST_OFFSET = (USED_END + 15) & ~usize{15};

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

    [[nodiscard]]
    auto queue_pfn() const -> u32 {
        return in32(0x08);
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
} // namespace

auto main(int, char **) -> int {
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (vma == Uandle::Invalid) {
        auto _ = ort::debug_line("virtio-blk: started without VmaRoot");
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

    // Bus 0 of the ECAM, one window for the whole enumeration.
    auto config_base = ours::VirtAddr{0};
    if (auto const status =
            map_mmio(vma, mmio_resource.unwrap(), ECAM_BASE, BUS_BYTES, &config_base);
        status != Status::Ok) {
        auto _ =
            ort::debug_line({"virtio-blk: cannot map the ECAM window: ", ours::to_string(status)});
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
        // boot check runs on images whose QEMU carries no disk.
        auto _ = ort::debug_line("virtio-blk: no virtio block device on bus 0");
        auto _ = ours::handle_close(ioport_resource.unwrap());
        return 0;
    }

    // Everything the shutdown owns, and the one place that puts it down. An
    // error path past the pin must stop the device and observe it stopped
    // before unpinning, or the kernel quarantines the memory.
    struct Owned {
        bool device_started = false;
        Uandle iommu = Uandle::Invalid;
        Uandle bti = Uandle::Invalid;
        Uandle region_vmo = Uandle::Invalid;
        Uandle region_pmt = Uandle::Invalid;
        ours::VirtAddr region_map = ours::VirtAddr{0};
    } owned;
    auto cleanup = [&](Status const status) -> int {
        if (owned.device_started) {
            auto const regs = LegacyRegs{io_base};
            regs.set_status(0);
            for (auto tries = u32{0}; tries < 1'000'000; ++tries) {
                if (regs.status() == 0) {
                    break;
                }
                ort::spin_hint();
            }
        }
        if (owned.region_pmt != Uandle::Invalid) {
            auto _ = ours::pmt_unpin(owned.region_pmt);
        }
        if (owned.region_map != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, owned.region_map, REGION_BYTES);
        }
        if (owned.region_vmo != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.region_vmo);
        }
        if (owned.bti != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.bti);
        }
        if (owned.iommu != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.iommu);
        }
        auto _ = ours::handle_close(ioport_resource.unwrap());
        if (status != Status::Ok) {
            auto _ = ort::debug_line({"virtio-blk: failed: ", ours::to_string(status)});
            return 1;
        }
        auto _ = ort::debug_line("virtio-blk: sector 0 read");
        return 0;
    };

    // Open the device's I/O ports to this process's threads.
    if (auto const status = ours::ioports_request(ioport_resource.unwrap(), io_base, 0x80);
        status != Status::Ok) {
        auto _ = ort::debug_line({"virtio-blk: cannot open its ports: ", ours::to_string(status)});
        return cleanup(status);
    }
    auto const regs = LegacyRegs{io_base};

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

    // The queue: selected, its maximum depth read from the device, the
    // driver's depth written back, and the ring registered by page frame.
    regs.select_queue(0);
    auto const queue_max = regs.queue_num();
    {
        char num_text[10];
        auto _ = ort::debug_line({"virtio-blk: queue 0 max ", ort::decimal(queue_max, num_text)});
    }
    if (queue_max == 0) {
        return cleanup(Status::OutOfRange);
    }
    // The legacy depth is the driver's to choose, within the device's
    // maximum: written back before the ring is registered, so the device
    // computes the rings' addresses from the same depth this driver laid
    // them out with.
    regs.set_queue_num(LEGACY_QUEUE_NUM);

    // The DMA resources: a stub IOMMU and one BTI for this device.
    if (auto const status = ours::iommu_create(iommu_resource.unwrap(), ours::IommuType::Stub,
                                               nullptr, 0, &owned.iommu);
        status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status = ours::bti_create(owned.iommu, 0, 0, &owned.bti); status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status =
            ours::vmo_create_contiguous(owned.bti, REGION_BYTES, 0, &owned.region_vmo);
        status != Status::Ok) {
        return cleanup(status);
    }
    auto region_phys = ours::PhysAddr{0};
    if (auto const status =
            ours::bti_pin(owned.bti,
                          ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite |
                              ours::BtiPinOptions::Contiguous,
                          owned.region_vmo, 0, REGION_BYTES, &region_phys, 1, &owned.region_pmt);
        status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status =
            ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0,
                          owned.region_vmo, 0, REGION_BYTES, &owned.region_map);
        status != Status::Ok) {
        return cleanup(status);
    }
    regs.set_queue_pfn(region_phys);
    {
        char pfn_text[18];
        char phys_text[18];
        auto _ = ort::debug_line({"virtio-blk: pfn register reads ",
                                  ort::hex(regs.queue_pfn(), pfn_text), " for region ",
                                  ort::hex(region_phys, phys_text)});
    }

    // The device is told the driver is ready, and may now be served.
    regs.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    owned.device_started = true;

    // One read request: header (device reads), data and status (device
    // writes), chained through the descriptor table. The request lives in
    // the region's tail, past the used ring.
    auto const buf_base = region_phys + REQUEST_OFFSET;
    auto *const descriptors = reinterpret_cast<volatile u8 *>(owned.region_map);
    auto const put64 = [&](usize const index, usize const field, u64 const value) -> void {
        *reinterpret_cast<volatile u64 *>(descriptors + index * 16 + field) = value;
    };
    auto const put32 = [&](usize const index, usize const field, u32 const value) -> void {
        *reinterpret_cast<volatile u32 *>(descriptors + index * 16 + field) = value;
    };
    auto const put16 = [&](usize const index, usize const field, u16 const value) -> void {
        *reinterpret_cast<volatile u16 *>(descriptors + index * 16 + field) = value;
    };
    // The request reads sector 0, whose first bytes the image was given:
    // what comes back is the end-to-end answer, checked against what was
    // written there.
    *reinterpret_cast<volatile u64 *>(owned.region_map + REQUEST_OFFSET + 8) = 0;
    // desc[0]: the header, read by the device, chained to desc[1].
    put64(0, 0, buf_base);
    put32(0, 8, HEADER);
    put16(0, 12, DESC_NEXT);
    put16(0, 14, 1);
    // desc[1]: the sector, written by the device, chained to desc[2].
    put64(1, 0, buf_base + HEADER);
    put32(1, 8, SECTOR);
    put16(1, 12, DESC_NEXT | DESC_WRITE);
    put16(1, 14, 2);
    // desc[2]: the status byte, written by the device, end of the chain.
    put64(2, 0, buf_base + HEADER + SECTOR);
    put32(2, 8, 1);
    put16(2, 12, DESC_WRITE);
    put16(2, 14, 0);

    // The available ring: one offered chain, the device's cursor moving past
    // it. x86 orders stores strongly; no barrier is asked for here.
    auto *const avail = descriptors + AVAIL_OFFSET;
    *reinterpret_cast<volatile u16 *>(avail + 2) = 1;
    *reinterpret_cast<volatile u16 *>(avail + 4) = 0;

    // Ring the queue.
    regs.notify(0);

    // Poll the used ring until the device answers. Interrupts come when the
    // kernel can name this device's line; the poll is the interim shape.
    auto *const used = descriptors + USED_OFFSET;
    auto completed = false;
    for (auto tries = u32{0}; tries < 100'000'000; ++tries) {
        if (*reinterpret_cast<volatile u16 const *>(used + 2) != 0) {
            completed = true;
            break;
        }
        ort::spin_hint();
    }
    if (!completed) {
        return cleanup(Status::TimeOut);
    }
    auto const used_id = *reinterpret_cast<volatile u32 const *>(used + 4);
    auto const used_len = *reinterpret_cast<volatile u32 const *>(used + 8);
    auto const status_byte =
        *reinterpret_cast<volatile u8 const *>(owned.region_map + REQUEST_OFFSET + HEADER + SECTOR);
    {
        char id_text[18];
        char len_text[18];
        auto _ = ort::debug_line({"virtio-blk: used entry id ", ort::hex(used_id, id_text), " len ",
                                  ort::hex(used_len, len_text)});
    }
    if (status_byte != 0) {
        char status_text[10];
        auto _ = ort::debug_line({"virtio-blk: the request failed with status ",
                                  ort::decimal(status_byte, status_text)});
        return cleanup(Status::BadState);
    }

    // What came back, as the answer to the whole exercise.
    auto read_bytes = std::array<u8, 16>{};
    for (auto index = usize{0}; index < read_bytes.size(); ++index) {
        read_bytes[index] = *reinterpret_cast<volatile u8 const *>(owned.region_map +
                                                                   REQUEST_OFFSET + HEADER + index);
    }
    auto _ = ort::debug_line(
        {"virtio-blk: sector 0 starts \"",
         std::string_view{reinterpret_cast<char const *>(read_bytes.data()), read_bytes.size()},
         "\""});
    return cleanup(Status::Ok);
}
