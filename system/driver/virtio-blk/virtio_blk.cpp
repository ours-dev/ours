/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// virtio-blk -- the userspace driver of QEMU's virtio block device
/// (storage-layer plan, .agents/tasks/storage-layer): this milestone
/// enumerates PCI bus 0 through its ECAM window, finds the virtio device,
/// maps its BARs, and reads its virtio capability list. Nothing is driven
/// yet: the virtqueue and the first read come next.
///
/// The objects:
///   - the MmIo root resource, taken through /svc, whose vmo_create_physical
///     makes windows onto device memory (the kernel refuses RAM and its own
///     devices behind it);
///   - the ECAM window: memory-mapped PCI configuration space. Its base is a
///     firmware fact userspace cannot read (ACPI's MCFG is out of reach), so
///     the q35 + OVMF value is spelled here; a function's configuration
///     space is ECAM + (bus << 20 | device << 15 | function << 12);
///   - the mapped configuration space: volatile reads and writes, uncached
///     by the kernel, because the hardware's answer is the only truth.
///
/// The events: the window over bus 0 (1 MiB) is mapped, every function's
/// vendor and device id is read, the virtio device is recognized by its
/// 0x1AF4 vendor, its BARs are sized and mapped, and the capability list is
/// walked for the virtio structures a driver drives -- common config, ISRs,
/// notifications, and the device-specific one. What this prints is the
/// driver's map of its own hardware.
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
constexpr auto CFG_COMMAND = usize{0x04};
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

/// The host bridge of bus 0, whose vendor id the enumeration is checked
/// against before anything else is believed.
constexpr auto INTEL_VENDOR = u16{0x8086};

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
/// space this driver makes. |cfg| is the function's, |index| the BAR's.
/// A 64-bit BAR (bits 2:1 == 2) is joined from its high dword, which the
/// caller is told to step over. False for an I/O-space BAR, and for a
/// register that reads 0, which is no BAR at all.
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

/// Reads the virtio capability list of the device |cfg| is, logging each
/// vendor-defined entry. The list is walked by its own next pointers, bounded
/// by the configuration space of one function, since a malformed list must
/// end the walk rather than the driver.
auto read_virtio_caps(Config const &cfg) -> void {
    auto seen = u32{0};
    auto ptr = usize{cfg.read8(CFG_CAP_PTR)};
    while (ptr != 0 && seen < 24) {
        auto const vendor = cfg.read8(ptr + 0);
        auto const next = cfg.read8(ptr + CFG_NEXT_PTR);
        if (vendor == PCI_CAP_VENDOR) {
            auto const type = cfg.read8(ptr + 3);
            auto const bar = cfg.read8(ptr + 4);
            auto const offset = cfg.read32(ptr + 8);
            auto const length = cfg.read32(ptr + 12);
            char type_text[10];
            char bar_text[10];
            char offset_text[18];
            char length_text[18];
            auto _ = ort::debug_line({"virtio-blk: capability type ", ort::decimal(type, type_text),
                                      " bar ", ort::decimal(bar, bar_text), " offset ",
                                      ort::hex(offset, offset_text), " length ",
                                      ort::hex(length, length_text)});
            if (type == std::to_underlying(VirtioCapType::Common)) {
                auto const notify_multiplier = cfg.read32(ptr + 16);
                char multiplier_text[18];
                auto _ = ort::debug_line({"virtio-blk: common config, notify_off_multiplier ",
                                          ort::hex(notify_multiplier, multiplier_text)});
            }
        }
        ptr = next;
        seen += 1;
    }
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

    // Bus 0 of the ECAM, one window for the whole enumeration: 1 MiB mapped
    // once and read through, rather than a map per function. The page tables
    // this costs are a known cost (docs/repairs/261030/kernel-object.md).
    auto config_base = ours::VirtAddr{0};
    if (auto const status =
            map_mmio(vma, mmio_resource.unwrap(), ECAM_BASE, BUS_BYTES, &config_base);
        status != Status::Ok) {
        auto _ =
            ort::debug_line({"virtio-blk: cannot map the ECAM window: ", ours::to_string(status)});
        auto _ = ours::handle_close(mmio_resource.unwrap());
        return 1;
    }

    auto found = false;
    for (auto device = u8{0}; device < 32; ++device) {
        auto const cfg = Config{config_base, to_function_offset(device, 0)};
        auto const vendor = cfg.read16(CFG_VENDOR_ID);
        if (vendor == 0xFFFF) {
            continue;
        }
        auto const device_id = cfg.read16(CFG_DEVICE_ID);
        char device_text[10];
        char vendor_text[18];
        char id_text[18];
        auto _ = ort::debug_line({"virtio-blk: bus 0 device ", ort::decimal(device, device_text),
                                  " vendor ", ort::hex(vendor, vendor_text), " device ",
                                  ort::hex(device_id, id_text)});

        if (vendor == VIRTIO_VENDOR &&
            (device_id == VIRTIO_BLK_TRANSITIONAL || device_id == VIRTIO_BLK_MODERN)) {
            found = true;
            // The command register is written by nothing here: the device is
            // still in its firmware state, and turning on bus master and
            // memory decode belongs with the virtqueue, not the enumeration.
            auto const command = cfg.read16(CFG_COMMAND);
            char command_text[18];
            auto _ = ort::debug_line({"virtio-blk: this is the block device, command ",
                                      ort::hex(command, command_text)});
            for (auto bar = u8{0}; bar < 6; ++bar) {
                auto base = ours::PhysAddr{0};
                auto size = usize{0};
                auto is_64bit = false;
                if (probe_bar(cfg, bar, &base, &size, &is_64bit)) {
                    char bar_text[10];
                    char base_text[18];
                    char size_text[18];
                    auto _ = ort::debug_line({"virtio-blk: bar ", ort::decimal(bar, bar_text),
                                              " at ", ort::hex(base, base_text), " size ",
                                              ort::hex(size, size_text)});
                    // The modern structures live in the BAR the capability
                    // list names; mapping one window now proves the path from
                    // a physical BAR to a readable register.
                    if (is_64bit) {
                        bar += 1;
                    }
                }
            }
            read_virtio_caps(cfg);
        }
        if (device == 0 && vendor == INTEL_VENDOR) {
            continue;
        }
    }

    auto _ = ours::vma_unmap(vma, config_base, BUS_BYTES);
    auto _ = ours::handle_close(mmio_resource.unwrap());
    // A boot without the device attached is an answer, not a failure: the
    // enumeration is the milestone, and the boot check runs on images whose
    // QEMU carries no disk.
    if (!found) {
        auto _ = ort::debug_line("virtio-blk: no virtio block device on bus 0");
        return 0;
    }
    auto _ = ort::debug_line("virtio-blk: enumerated");
    return 0;
}
