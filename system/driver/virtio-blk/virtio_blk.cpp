/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// virtio-blk -- the userspace driver of QEMU's virtio block device
/// (storage-layer plan, .agents/tasks/storage-layer). This milestone drives
/// the device for the first time: it enumerates PCI bus 0 through its ECAM
/// window, finds the block device, brings the modern interface up, sets the
/// split virtqueue up in pinned memory, submits a read of sector 0 through
/// it, and polls the used ring for the answer.
///
/// The objects:
///   - the MmIo root resource, through /svc: vmo_create_physical makes the
///     windows onto the ECAM and the device's BARs, uncached and fully
///     populated;
///   - the IOMMU System resource: a stub IOMMU, a BTI for the device, and
///     the pins that hand the rings and the buffers to the device by
///     physical address;
///   - the common config: the structure the modern capability list names,
///     where the device is reset, features are negotiated, and the queue is
///     registered;
///   - the split virtqueue: descriptor table, available ring and used ring
///     in one pinned page, the request header, data and status in another.
///
/// The events: reset (status 0, read back), ACKNOWLEDGE, DRIVER, feature
/// negotiation (VERSION_1), FEATURES_OK, queue registration (desc, avail,
/// used by physical address, then enable), DRIVER_OK, one descriptor chain
/// offered through the available ring, a doorbell write, and the poll.
/// Before any pin is undone the device is stopped and observed stopped, so
/// the kernel never has to quarantine what this driver pinned; every error
/// path goes through that same shutdown.
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

/// One bus of configuration space: 32 devices, 8 functions, 4 KiB each.
constexpr auto BUS_BYTES = usize{1 << 20};

/// PCI configuration-space registers this driver reads, in bytes from the
/// function's base.
constexpr auto CFG_VENDOR_ID = usize{0x00};
constexpr auto CFG_DEVICE_ID = usize{0x02};
constexpr auto CFG_BAR_0 = usize{0x10};
constexpr auto CFG_CAP_PTR = usize{0x34};
constexpr auto CFG_NEXT_PTR = usize{0x01};

/// A capability list entry: vendor@0, next@1, and for the vendor-defined
/// ones the virtio fields that follow.
constexpr auto PCI_CAP_VENDOR = u8{0x09};

/// The virtio capability types, in the order the specification numbers them.
enum class VirtioCapType : u8 {
    Common = 1,
    Notify = 2,
    Isr = 3,
    Device = 4,
};

/// The virtio vendor, and the device ids of interest: the transitional block
/// device QEMU makes by default, and its modern (virtio 1.0) form.
constexpr auto VIRTIO_VENDOR = u16{0x1AF4};
constexpr auto VIRTIO_BLK_TRANSITIONAL = u16{0x1001};
constexpr auto VIRTIO_BLK_MODERN = u16{0x1042};

/// The device status bits, in the order the specification demands them.
constexpr auto STATUS_ACKNOWLEDGE = u8{1};
constexpr auto STATUS_DRIVER = u8{2};
constexpr auto STATUS_DRIVER_OK = u8{4};
constexpr auto STATUS_FEATURES_OK = u8{8};

/// VIRTIO_F_VERSION_1, feature bit 32: the second feature word's bit 0.
constexpr auto VERSION_1_WORD = u32{1};
constexpr auto VERSION_1_BIT = u32{1};

/// A descriptor's flags, and a request's type: a read hands the data and the
/// status to the device to write.
constexpr auto DESC_NEXT = u16{1};
constexpr auto DESC_WRITE = u16{2};

/// One page, the unit the BTI pins in and the granularity the VMOs ask for.
constexpr auto PAGE = usize{4096};

/// One sector, the block device's unit, plus the 16-byte request header that
/// precedes the data and the 1-byte status that follows it in a request.
constexpr auto SECTOR = usize{512};
constexpr auto HEADER = usize{16};
constexpr auto REQUEST_BYTES = HEADER + SECTOR + 1;

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
    auto read8(usize const offset) const -> u8 {
        return *reinterpret_cast<volatile u8 const *>(m_base + m_offset + offset);
    }

    [[nodiscard]]
    auto read32(usize const offset) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_base + m_offset + offset);
    }

    auto write32(usize const offset, u32 const value) const -> void {
        *reinterpret_cast<volatile u32 *>(m_base + m_offset + offset) = value;
    }

  private:
    ours::VirtAddr m_base;
    usize m_offset;
};

auto to_function_offset(u8 const device, u8 const function) -> usize {
    return (usize{device} << 15) | (usize{function} << 12);
}

/// The memory BAR |bar| names: its physical base, and its size as the
/// firmware mapped it. Sizing writes the all-ones pattern to the register and
/// reads back what the hardware kept, which is the complement of the size --
/// the PCI specification's own probe, and the one write to configuration
/// space this driver makes. A 64-bit BAR (bits 2:1 == 2) is joined from its
/// high dword, which the caller is told to step over. False for an I/O-space
/// BAR, and for a register that reads 0, which is no BAR at all.
auto probe_bar(Config const &cfg, u8 const index, ours::PhysAddr *out_base, usize *out_size,
               bool *out_is_64bit) -> bool {
    auto const reg = CFG_BAR_0 + usize{index} * 4;
    auto const low = cfg.read32(reg);
    if (low == 0) {
        return false;
    }
    if ((low & 1) != 0) {
        return false;
    }
    auto const is_64bit = ((low >> 1) & 3) == 2;
    cfg.write32(reg, 0xFFFF'FFFF);
    auto const masked = cfg.read32(reg);
    cfg.write32(reg, low);
    if (masked == 0) {
        return false;
    }
    // The complement is taken in the register's own width: widening before
    // flipping would turn the high zeros of the u32 into ones.
    auto const size = usize{static_cast<u32>(~(masked & 0xFFFF'FFF0u))} + 1;
    auto base = u64{low & 0xFFFF'FFF0u};
    if (is_64bit && index < 5) {
        auto const high = cfg.read32(reg + 4);
        base |= static_cast<u64>(high) << 32;
    }
    *out_base = ours::PhysAddr{base};
    *out_size = size;
    *out_is_64bit = is_64bit;
    return true;
}

/// One virtio structure of the modern interface, as the capability list
/// names it: which BAR, where in it, how long.
struct Capability {
    VirtioCapType type;
    u8 bar;
    u32 offset;
    u32 length;
    u32 notify_multiplier;
};

/// Collects the vendor-defined capabilities of the device |cfg| is. The list
/// is walked by its own next pointers, bounded by the configuration space of
/// one function, since a malformed list must end the walk rather than the
/// driver. |out| is filled in order of type; the multiplier is only defined
/// for the notify structure.
auto read_virtio_caps(Config const &cfg, Capability *out) -> usize {
    auto count = usize{0};
    auto seen = u32{0};
    auto ptr = usize{cfg.read8(CFG_CAP_PTR)};
    while (ptr != 0 && seen < 24) {
        if (cfg.read8(ptr + 0) == PCI_CAP_VENDOR) {
            auto const cap = Capability{
                .type = static_cast<VirtioCapType>(cfg.read8(ptr + 3)),
                .bar = cfg.read8(ptr + 4),
                .offset = cfg.read32(ptr + 8),
                .length = cfg.read32(ptr + 12),
                .notify_multiplier = cfg.read32(ptr + 16),
            };
            char type_text[10];
            char bar_text[10];
            char offset_text[18];
            char length_text[18];
            char multiplier_text[18];
            auto _ = ort::debug_line({"virtio-blk: capability type ",
                                      ort::decimal(std::to_underlying(cap.type), type_text),
                                      " bar ", ort::decimal(cap.bar, bar_text), " offset ",
                                      ort::hex(cap.offset, offset_text), " length ",
                                      ort::hex(cap.length, length_text), " multiplier ",
                                      ort::hex(cap.notify_multiplier, multiplier_text)});
            if (count < 5) {
                out[count] = cap;
                count += 1;
            }
        }
        ptr = cfg.read8(ptr + CFG_NEXT_PTR);
        seen += 1;
    }
    return count;
}

/// Maps [paddr, paddr + size) into this process with the MmIo root, through a
/// physical VMO and the startup VMA. Uncached, as every mapping of a physical
/// VMO is; a device register is about to be read through it.
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

/// The common config structure, at its capability's offset in its BAR. Every
/// access is volatile: the device's answer is the only truth, and the
/// compiler may not fold or reorder it.
class CommonConfig final {
  public:
    CommonConfig(ours::VirtAddr bar_base, u32 const offset) : m_base(bar_base + offset) {}

    // Every access is at its own field's offset and natural width, and the
    // one exception is a 64-bit field, read as the two u32 halves the
    // specification lays out. The device answers an access that spans two
    // fields with the value of one of them, so a wider read is not a
    // shorthand here, and only the u64 fields span more than one access.
    [[nodiscard]]
    auto read32(usize const offset) const -> u32 {
        return *reinterpret_cast<volatile u32 const *>(m_base + offset);
    }

    [[nodiscard]]
    auto read16(usize const offset) const -> u16 {
        return *reinterpret_cast<volatile u16 const *>(m_base + offset);
    }

    [[nodiscard]]
    auto read8(usize const offset) const -> u8 {
        return *reinterpret_cast<volatile u8 const *>(m_base + offset);
    }

    [[nodiscard]]
    auto read64(usize const offset) const -> u64 {
        return static_cast<u64>(read32(offset)) | (static_cast<u64>(read32(offset + 4)) << 32);
    }

    auto write32(usize const offset, u32 const value) const -> void {
        *reinterpret_cast<volatile u32 *>(m_base + offset) = value;
    }

    // The narrow writes stay narrow: the device dispatches each field by its
    // own offset, so a wide write that spans two of them lands in one and is
    // refused by the other. The composed reads are the exception that works,
    // because a read's value is whatever the field holds.
    auto write16(usize const offset, u16 const value) const -> void {
        *reinterpret_cast<volatile u16 *>(m_base + offset) = value;
    }

    auto write8(usize const offset, u8 const value) const -> void {
        *reinterpret_cast<volatile u8 *>(m_base + offset) = value;
    }

    auto write64(usize const offset, u64 const value) const -> void {
        write32(offset, static_cast<u32>(value));
        write32(offset + 4, static_cast<u32>(value >> 32));
    }

  private:
    ours::VirtAddr m_base;
};

/// The common config's registers, in bytes from its offset.
// The common config always carries the MSI-X fields at 0x10 and 0x12, even
// where MSI-X itself is unused: every register past them sits four bytes
// further than the MSI-X-less layout would put it.
constexpr auto CC_DEVICE_FEATURE_SELECT = usize{0x00};
constexpr auto CC_DEVICE_FEATURE = usize{0x04};
constexpr auto CC_DRIVER_FEATURE_SELECT = usize{0x08};
constexpr auto CC_DRIVER_FEATURE = usize{0x0C};
constexpr auto CC_STATUS = usize{0x16};
constexpr auto CC_QUEUE_SELECT = usize{0x18};
constexpr auto CC_QUEUE_SIZE = usize{0x1A};
constexpr auto CC_QUEUE_ENABLE = usize{0x1E};
constexpr auto CC_QUEUE_NOTIFY_OFF = usize{0x20};
constexpr auto CC_QUEUE_DESC = usize{0x24};
constexpr auto CC_QUEUE_AVAIL = usize{0x2C};
constexpr auto CC_QUEUE_USED = usize{0x34};

/// Stops the device and waits until it says so. The precondition of every
/// pmt_unpin: a pin undone while the device might still be writing makes the
/// kernel quarantine the memory, and this driver must never cause that.
auto stop_device(CommonConfig const &common) -> bool {
    common.write8(CC_STATUS, 0);
    for (auto tries = u32{0}; tries < 1'000'000; ++tries) {
        if (common.read8(CC_STATUS) == 0) {
            return true;
        }
        ort::spin_hint();
    }
    return false;
}

/// Pins one page of |vmo| for the device, read and write. The stub BTI has
/// page-sized contiguity, so a page pins to exactly one address; the count is
/// passed as exactly 1, which is what the default (one address per page)
/// answers.
auto pin_page(Uandle const bti, Uandle const vmo, ours::PhysAddr *out, Uandle *out_pmt) -> Status {
    constexpr auto RW = ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite;
    return ours::bti_pin(bti, RW, vmo, 0, PAGE, out, 1, out_pmt);
}
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
    auto const iommu_resource = ours_resource::get(ours_resource::IOMMU_PROTOCOL);
    if (iommu_resource.is_err()) {
        auto _ = ort::debug_line(
            {"virtio-blk: no Iommu resource: ", ours::to_string(iommu_resource.unwrap_err())});
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }

    // Bus 0 of the ECAM, one window for the whole enumeration: 1 MiB mapped
    // once and read through, rather than a map per function. The page tables
    // this costs are a known cost (docs/repairs/261030/kernel-object.md).
    auto config_base = ours::VirtAddr{0};
    if (auto const status =
            map_mmio(vma, mmio_resource.unwrap(), ECAM_BASE, BUS_BYTES, &config_base);
        status != Status::Ok) {
        auto _ =
            ort::debug_line({"virtio-blk: cannot map the ECAM window: ", ours::to_string(status)});
        auto _ = ours::handle_close(iommu_resource.unwrap());
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }

    // The block device, and its modern structures once the capability list
    // has named them.
    auto device_offset = usize{0};
    auto caps = std::array<Capability, 5>{};
    auto num_caps = usize{0};
    for (auto device = u8{0}; device < 32 && device_offset == 0; ++device) {
        auto const cfg = Config{config_base, to_function_offset(device, 0)};
        auto const vendor = cfg.read16(CFG_VENDOR_ID);
        if (vendor == 0xFFFF) {
            continue;
        }
        auto const device_id = cfg.read16(CFG_DEVICE_ID);
        if (vendor != VIRTIO_VENDOR ||
            (device_id != VIRTIO_BLK_TRANSITIONAL && device_id != VIRTIO_BLK_MODERN)) {
            continue;
        }
        device_offset = to_function_offset(device, 0);
        num_caps = read_virtio_caps(cfg, caps.data());
    }

    // The device is found. Its BAR is probed while the ECAM window is still
    // mapped: the probe writes the BAR registers themselves.
    auto common_cap = usize{0};
    auto notify_cap = usize{0};
    for (auto index = usize{0}; index < num_caps; ++index) {
        if (caps[index].type == VirtioCapType::Common) {
            common_cap = index;
        }
        if (caps[index].type == VirtioCapType::Notify) {
            notify_cap = index;
        }
    }
    if (common_cap == num_caps || notify_cap == num_caps) {
        auto _ = ort::debug_line("virtio-blk: the modern structures are not all there");
        auto _ = ours::handle_close(iommu_resource.unwrap());
        return 1;
    }
    auto device_cfg = Config{config_base, device_offset};

    // Everything the shutdown owns, and the one place that puts it down. An
    // error path past the first pin must stop the device and observe it
    // stopped before unpinning, or the kernel quarantines the memory; a
    // device that was never started needs no stop.
    struct Owned {
        bool device_started = false;
        Uandle iommu = Uandle::Invalid;
        Uandle bti = Uandle::Invalid;
        Uandle ring_vmo = Uandle::Invalid;
        Uandle ring_pmt = Uandle::Invalid;
        Uandle buf_vmo = Uandle::Invalid;
        Uandle buf_pmt = Uandle::Invalid;
        ours::VirtAddr ring_map = ours::VirtAddr{0};
        ours::VirtAddr buf_map = ours::VirtAddr{0};
        ours::VirtAddr bar_map = ours::VirtAddr{0};
        usize bar_bytes = 0;
    } owned;
    auto cleanup = [&](Status const status) -> int {
        auto common = CommonConfig{owned.bar_map, caps[common_cap].offset};
        if (owned.device_started && owned.bar_bytes != 0) {
            auto _ = stop_device(common);
        }
        if (owned.ring_pmt != Uandle::Invalid) {
            auto _ = ours::pmt_unpin(owned.ring_pmt);
        }
        if (owned.buf_pmt != Uandle::Invalid) {
            auto _ = ours::pmt_unpin(owned.buf_pmt);
        }
        if (owned.ring_map != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, owned.ring_map, PAGE);
        }
        if (owned.buf_map != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, owned.buf_map, PAGE);
        }
        if (owned.bar_map != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, owned.bar_map, owned.bar_bytes);
        }
        if (owned.ring_vmo != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.ring_vmo);
        }
        if (owned.buf_vmo != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.buf_vmo);
        }
        if (owned.bti != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.bti);
        }
        if (owned.iommu != Uandle::Invalid) {
            auto _ = ours::handle_close(owned.iommu);
        }
        auto _ = ours::handle_close(iommu_resource.unwrap());
        if (status != Status::Ok) {
            auto _ = ort::debug_line({"virtio-blk: failed: ", ours::to_string(status)});
            return 1;
        }
        auto _ = ort::debug_line("virtio-blk: sector 0 read");
        return 0;
    };

    auto bar_base = ours::PhysAddr{0};
    auto bar_bytes = usize{0};
    {
        auto is_64bit = false;
        if (auto const probed =
                probe_bar(device_cfg, caps[common_cap].bar, &bar_base, &bar_bytes, &is_64bit);
            !probed) {
            return cleanup(Status::NotSupported);
        }
    }
    auto _ = ours::vma_unmap(vma, config_base, BUS_BYTES);

    if (auto const status =
            map_mmio(vma, mmio_resource.unwrap(), bar_base, bar_bytes, &owned.bar_map);
        status != Status::Ok) {
        return cleanup(status);
    }
    owned.bar_bytes = bar_bytes;
    auto common = CommonConfig{owned.bar_map, caps[common_cap].offset};

    // The queue's size, read before the reset: the device forgets its rings
    // when it is reset, and the size is the device's to say, never the
    // driver's to write.
    auto const queue_size = common.read16(CC_QUEUE_SIZE);
    {
        char size_text[10];
        auto _ = ort::debug_line(
            {"virtio-blk: queue 0 size at firmware state ", ort::decimal(queue_size, size_text)});
    }

    // Bring-up, in the order the specification fixes: reset, acknowledge,
    // driver, features, then the queue.
    if (!stop_device(common)) {
        return cleanup(Status::TimeOut);
    }
    common.write8(CC_STATUS, STATUS_ACKNOWLEDGE);
    common.write8(CC_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // VERSION_1 decides which interface this is; without it this driver has
    // no path, since the structures it found are the modern ones.
    common.write32(CC_DEVICE_FEATURE_SELECT, 0);
    auto const features_low = common.read32(CC_DEVICE_FEATURE);
    common.write32(CC_DEVICE_FEATURE_SELECT, VERSION_1_WORD);
    auto const device_features = common.read32(CC_DEVICE_FEATURE);
    {
        char low_text[18];
        char high_text[18];
        char status_text[10];
        auto _ =
            ort::debug_line({"virtio-blk: device features low ", ort::hex(features_low, low_text),
                             " high ", ort::hex(device_features, high_text), " status ",
                             ort::decimal(common.read8(CC_STATUS), status_text)});
    }
    if ((device_features & VERSION_1_BIT) == 0) {
        return cleanup(Status::NotSupported);
    }
    common.write32(CC_DRIVER_FEATURE_SELECT, VERSION_1_WORD);
    common.write32(CC_DRIVER_FEATURE, VERSION_1_BIT);
    common.write8(CC_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if ((common.read8(CC_STATUS) & STATUS_FEATURES_OK) == 0) {
        return cleanup(Status::NotSupported);
    }

    // The DMA resources: a stub IOMMU, one BTI for this device. The stub
    // translates nothing, so a pinned address is the physical address, and
    // the bti_id only names the device in the kernel's books.
    if (auto const status = ours::iommu_create(iommu_resource.unwrap(), ours::IommuType::Stub,
                                               nullptr, 0, &owned.iommu);
        status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status = ours::bti_create(owned.iommu, 0, 0, &owned.bti); status != Status::Ok) {
        return cleanup(status);
    }

    // Queue 0. Its tables fit one page for every queue size this device can
    // ask for: descriptors, the available ring and the used ring together.
    // The device config's capacity, the one field whose truth is known
    // independently: a 64 MiB disk is 0x20000 sectors of 512 bytes. If this
    // reads wrong, the reads through the BAR are not the device's.
    auto words = std::array<char, 99>{};
    auto at = usize{0};
    for (auto word = usize{0}; word < 16; ++word) {
        auto const value = common.read32(word * 4);
        constexpr auto DIGITS = "0123456789ABCDEF";
        for (auto nibble = usize{0}; nibble < 8; ++nibble) {
            words[at + 7 - nibble] = DIGITS[(value >> (nibble * 4)) & 0xF];
        }
        at += 8;
        words[at] = ' ';
        at += 1;
    }
    words[at] = 0;
    auto _ = ort::debug_line({"virtio-blk: common config words: ", words.data()});
    for (auto index = usize{0}; index < num_caps; ++index) {
        if (caps[index].type != VirtioCapType::Device) {
            continue;
        }
        auto const device_config = owned.bar_map + caps[index].offset;
        char low_text[18];
        char high_text[18];
        auto _ = ort::debug_line(
            {"virtio-blk: blk config capacity low ",
             ort::hex(*reinterpret_cast<volatile u32 const *>(device_config), low_text), " high ",
             ort::hex(*reinterpret_cast<volatile u32 const *>(device_config + 4), high_text)});
    }
    common.write16(CC_QUEUE_SELECT, 0);
    auto const avail_offset = usize{queue_size} * 16;
    auto const used_offset = (avail_offset + 6 + usize{queue_size} * 2 + 3) & ~usize{3};
    auto const used_end = used_offset + 6 + usize{queue_size} * 8;
    if (queue_size == 0 || used_end > PAGE) {
        return cleanup(Status::OutOfRange);
    }
    if (auto const status = ours::vmo_create(PAGE, ours::VmoOptions::None, &owned.ring_vmo);
        status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status = ours::vmo_create(REQUEST_BYTES, ours::VmoOptions::None, &owned.buf_vmo);
        status != Status::Ok) {
        return cleanup(status);
    }

    // The rings and the request live at physical addresses the device reads
    // and writes, and in this address space where the driver fills them in.
    auto ring_phys = ours::PhysAddr{0};
    if (auto const status = pin_page(owned.bti, owned.ring_vmo, &ring_phys, &owned.ring_pmt);
        status != Status::Ok) {
        return cleanup(status);
    }
    auto buf_phys = ours::PhysAddr{0};
    if (auto const status = pin_page(owned.bti, owned.buf_vmo, &buf_phys, &owned.buf_pmt);
        status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status =
            ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0,
                          owned.ring_vmo, 0, PAGE, &owned.ring_map);
        status != Status::Ok) {
        return cleanup(status);
    }
    if (auto const status =
            ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0,
                          owned.buf_vmo, 0, REQUEST_BYTES, &owned.buf_map);
        status != Status::Ok) {
        return cleanup(status);
    }

    // Register the queue by physical address, then turn it on.
    common.write64(CC_QUEUE_DESC, ring_phys);
    common.write64(CC_QUEUE_AVAIL, ring_phys + avail_offset);
    common.write64(CC_QUEUE_USED, ring_phys + used_offset);
    common.write16(CC_QUEUE_ENABLE, 1);

    // One read request: header (device reads), data and status (device
    // writes), chained through the descriptor table.
    auto *const descriptors = reinterpret_cast<volatile u8 *>(owned.ring_map);
    auto const descriptor = [&](usize const index)->volatile u32 * {
        return reinterpret_cast<volatile u32 *>(descriptors + index * 16);
    };
    auto const put64 = [&](usize const index, usize const field, u64 const value) -> void {
        auto *const word = reinterpret_cast<volatile u64 *>(descriptors + index * 16 + field);
        *word = value;
    };
    auto const put32 = [&](usize const index, usize const field, u32 const value) -> void {
        *descriptor(index) = value;
    };
    auto const put16 = [&](usize const index, usize const field, u16 const value) -> void {
        *reinterpret_cast<volatile u16 *>(descriptors + index * 16 + field) = value;
    };
    // desc[0]: the header, read by the device, chained to desc[1].
    put64(0, 0, buf_phys);
    put32(0, 8, HEADER);
    put16(0, 12, DESC_NEXT);
    put16(0, 14, 1);
    // desc[1]: the sector, written by the device, chained to desc[2].
    put64(1, 0, buf_phys + HEADER);
    put32(1, 8, SECTOR);
    put16(1, 12, DESC_NEXT | DESC_WRITE);
    put16(1, 14, 2);
    // desc[2]: the status byte, written by the device, end of the chain.
    put64(2, 0, buf_phys + HEADER + SECTOR);
    put32(2, 8, 1);
    put16(2, 12, DESC_WRITE);
    put16(2, 14, 0);

    // The available ring: one offered chain, the device's cursor moving past
    // it. x86 orders stores strongly, and the doorbell is an uncached write;
    // no barrier is asked for here.
    auto *const avail = descriptors + avail_offset;
    *reinterpret_cast<volatile u16 *>(avail + 2) = 1;
    *reinterpret_cast<volatile u16 *>(avail + 4) = 0;

    // The device may now be served, and may now write the buffers.
    common.write8(CC_STATUS,
                  STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    owned.device_started = true;

    // The doorbell: the notify structure's offset in its BAR, plus this
    // queue's offset in the notify region scaled by the capability's
    // multiplier, written the queue's index.
    auto *const doorbell = reinterpret_cast<volatile u16 *>(
        owned.bar_map + caps[notify_cap].offset +
        usize{common.read16(CC_QUEUE_NOTIFY_OFF)} * caps[notify_cap].notify_multiplier);
    *doorbell = 0;

    // Poll the used ring until the device answers. Interrupts come when the
    // kernel can name this device's line; the poll is the interim shape.
    auto const used = descriptors + used_offset;
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
        *reinterpret_cast<volatile u8 const *>(owned.buf_map + HEADER + SECTOR);
    char id_text[18];
    char len_text[18];
    auto _ = ort::debug_line({"virtio-blk: used entry id ", ort::hex(used_id, id_text), " len ",
                              ort::hex(used_len, len_text)});
    if (status_byte != 0) {
        char status_text[10];
        auto _ = ort::debug_line({"virtio-blk: the request failed with status ",
                                  ort::decimal(status_byte, status_text)});
        return cleanup(Status::BadState);
    }

    // What came back, as the answer to the whole exercise.
    auto read_bytes = std::array<u8, 16>{};
    for (auto index = usize{0}; index < read_bytes.size(); ++index) {
        read_bytes[index] = *reinterpret_cast<volatile u8 const *>(owned.buf_map + HEADER + index);
    }
    auto _ = ort::debug_line(
        {"virtio-blk: sector 0 starts \"",
         std::string_view{reinterpret_cast<char const *>(read_bytes.data()), read_bytes.size()},
         "\""});
    return cleanup(Status::Ok);
}
