#include <ours/boot/handoff.hpp>

#include <ours/mm/init.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/panic.hpp>

namespace ours::boot {
namespace {
    phys::Handoff const *g_handoff;

    auto handoff_fits_physmap(PhysAddr const base) -> bool {
        if (base < mm::PhysMap::kPhysBase) {
            return false;
        }
        auto const offset = base - mm::PhysMap::kPhysBase;
        return offset <= mm::PhysMap::kSize && sizeof(phys::Handoff) <= mm::PhysMap::kSize - offset;
    }

    auto boot_package_from_handoff(phys::BootPackageHandoff const &record)
        -> phys::BootObi::Result {
        if (!record.fits_within(mm::PhysMap::kPhysBase, mm::PhysMap::kSize)) {
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
    if (!value->boot_package.fits_within(mm::PhysMap::kPhysBase, mm::PhysMap::kSize)) {
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
    g_handoff = value;
}

auto handoff() -> phys::Handoff const & {
    if (g_handoff == nullptr) {
        panic("Kernel handoff has not been installed");
    }
    return *g_handoff;
}

auto boot_package() -> phys::BootObi::Result {
    return boot_package_from_handoff(handoff().boot_package);
}

auto kernel_package(ustl::views::Span<u8> buffer) -> phys::KernelPackage::Result {
    auto const package = boot_package();
    if (package.is_err()) {
        return ustl::err(package.unwrap_err());
    }
    return package.unwrap().kernel_package(buffer);
}
} // namespace ours::boot
