#include <ours/boot/handoff.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/panic.hpp>

namespace ours::boot {
namespace {
    phys::Handoff const *HANDOFF;

    /// Whether the boot image is still the kernel's to read. Written once, by
    /// take_boot_image(), from userboot_init() on the laborer thread -- after
    /// userboot's own thread has started, though no system call reads the
    /// image. Read by boot_package(): on the boot thread (setup_options())
    /// before the laborer is spawned, which orders those reads before the
    /// write, and on the laborer itself. So plain is enough.
    enum class BootImage : u8 {
        Held,
        Taken,
    };
    BootImage BOOT_IMAGE = BootImage::Held;

    auto handoff_fits_physmap(PhysAddr const base) -> bool {
        if (base < mm::PhysMap::PHYS_BASE) {
            return false;
        }
        auto const offset = base - mm::PhysMap::PHYS_BASE;
        return offset <= mm::PhysMap::SIZE && sizeof(phys::Handoff) <= mm::PhysMap::SIZE - offset;
    }

    auto boot_package_from_handoff(phys::BootPackageHandoff const &record)
        -> phys::BootObi::Result {
        if (!record.fits_within(mm::PhysMap::PHYS_BASE, mm::PhysMap::SIZE)) {
            return ustl::err(Status::BadState);
        }

        auto const *const bytes = mm::PhysMap::phys_to_virt<u8 const>(record.package.base);
        if (bytes == nullptr) {
            return ustl::err(Status::BadState);
        }
        auto const container = obi::ContainerView::from(bytes, record.package.size);
        if (container.is_err()) {
            return ustl::err(container.unwrap_err());
        }
        return phys::BootObi::from(container.unwrap());
    }
} // namespace

auto setup_handoff(PhysAddr const handoff_phys) -> void {
    if (handoff_phys == 0 || !handoff_fits_physmap(handoff_phys)) {
        panic("Kernel handoff is outside the physmap");
    }

    auto const *const value = mm::PhysMap::phys_to_virt<phys::Handoff>(handoff_phys);
    if (value == nullptr || !value->verify()) {
        panic("Kernel handoff is invalid");
    }
    if (!value->boot_package.fits_within(mm::PhysMap::PHYS_BASE, mm::PhysMap::SIZE)) {
        panic("Boot package is outside the physmap");
    }
    auto const package = boot_package_from_handoff(value->boot_package);
    if (package.is_err()) {
        panic("Boot package handoff does not match OBI payloads");
    }

    auto const status = mm::handoff_early_pmm(value->mem);
    if (status != Status::Ok) {
        panic("Failed to accept kernel handoff");
    }
    HANDOFF = value;
}

auto handoff() -> phys::Handoff const & {
    if (HANDOFF == nullptr) {
        panic("Kernel handoff has not been installed");
    }
    return *HANDOFF;
}

auto boot_package() -> phys::BootObi::Result {
    if (BOOT_IMAGE == BootImage::Taken) {
        panic("the boot image was read after it was handed to userboot");
    }
    return boot_package_from_handoff(handoff().boot_package);
}

auto take_boot_image() -> phys::PackagePayloadHandoff {
    ASSERT(BOOT_IMAGE == BootImage::Held, "the boot image was taken twice");
    BOOT_IMAGE = BootImage::Taken;
    return handoff().boot_package.package;
}

auto kernel_package() -> phys::KernelPackage::Result {
    auto const package = boot_package();
    if (package.is_err()) {
        return ustl::err(package.unwrap_err());
    }
    auto const item = package.unwrap().kernel_package_item();
    if (item.is_err()) {
        return ustl::err(item.unwrap_err());
    }
    // setup_handoff() checked the range lies inside the physmap.
    auto const &storage = handoff().boot_package.kernel_storage;
    if (!storage.present) {
        return ustl::err(Status::NotFound);
    }
    auto const *const bytes = mm::PhysMap::phys_to_virt<u8 const>(storage.base);
    if (bytes == nullptr) {
        return ustl::err(Status::BadState);
    }
    return phys::KernelPackage::view(item.unwrap(), {bytes, storage.size});
}
} // namespace ours::boot
