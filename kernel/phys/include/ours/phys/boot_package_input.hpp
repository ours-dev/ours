/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BOOT_PACKAGE_INPUT_HPP
#define OURS_PHYS_BOOT_PACKAGE_INPUT_HPP 1

#include <ours/phys/arch_bootmem.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <limits>

#include <ustl/mem/align.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace ours::phys {
/// A validated physical OBI package range supplied by the boot loader.
/// `MemoryProvisioner::reserve_package` reserves it immediately after
/// memory discovery and before any BootMem allocation can overwrite its
/// nested payloads.
///
/// The base must be page-aligned, and the reservation covers whole pages:
/// the kernel hands the image's own frames to userboot in a VMO
/// (.agents/tasks/boot-image-pages), and a frame can belong to one owner
/// only. Zircon's physboot keeps the data ZBI the same way, page-aligned and
/// rounded out to a page (MakePhysVmo() in zircon/kernel/phys/handoff-prep.cc).
/// The bytes themselves, bytes(), stay exactly the image: an OBI container
/// view checks its size against them.
class BootPackageInput final {
  public:
    using Result = ustl::Result<BootPackageInput, Status>;

    [[nodiscard]]
    static auto from(PhysAddr const base, usize const size) -> Result {
        if (base == 0 || size == 0) {
            return ustl::err(Status::InvArgs);
        }
        // Multiboot already refuses an unaligned module (multiboot::View::
        // module()), so this cannot fire through the one boot path there is;
        // it is said here because the whole-page reservation below depends
        // on it, whatever loads the image.
        if (base % PAGE_SIZE != 0) {
            return ustl::err(Status::InvArgs);
        }
        if (size > std::numeric_limits<usize>::max() - (PAGE_SIZE - 1) ||
            base > std::numeric_limits<PhysAddr>::max() - ustl::mm::align_up(size, PAGE_SIZE)) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(BootPackageInput(base, size));
    }

    [[nodiscard]]
    auto bytes() const -> ustl::views::Span<u8 const> {
        return {reinterpret_cast<u8 const *>(base_), size_};
    }

    /// Reserves the image rounded out to whole pages, without touching the
    /// slack after its last byte. Zeroing it is the kernel's job, once ACPI
    /// has read the RSDP: that pointer points into the Multiboot info block,
    /// which nothing reserves and which may share the image's last page
    /// (docs/repairs/261003/x86-phys.md).
    [[nodiscard]]
    auto reserve(BootMem &memory) const -> Status {
        auto const range = bootmem::Range::from(base_, ustl::mm::align_up(size_, PAGE_SIZE));
        if (range.is_err()) {
            return range.unwrap_err();
        }
        return memory.reserve(range.unwrap(), bootmem::MemoryType::BootPackage);
    }

  private:
    BootPackageInput(PhysAddr const base, usize const size) : base_(base), size_(size) {}

    PhysAddr base_;
    usize size_;
};
} // namespace ours::phys

#endif // OURS_PHYS_BOOT_PACKAGE_INPUT_HPP
