/// physboot — the default phys module that boots the Ours kernel (vmours)
///
/// physload loads this module out of the kernel package, binds its imports
/// against physload's export table and runs its static constructors, then
/// calls the entry declared by PHYS_MODULE() at the bottom of this file.
/// From there physboot resolves the kernel ELF inside the `kernel.name`
/// BootFS directory, loads it, applies KASLR relocation, and hands off to
/// the kernel.

#include "physboot.hpp"

#include <ours/phys/aspace.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/kernel-boot.hpp>
#include <ours/phys/kernel-image.hpp>
#include <ours/phys/kernel-package-loader.hpp>
#include <ours/phys/module.hpp>
#include <ours/phys/print.hpp>

#include <string_view>

namespace ours::phys {
namespace {
    using std::operator""sv;

    FORCE_INLINE auto patch_kernel(KernelImage &image) -> void {
        // TODO: Apply code-patching metadata (<kernel_name>/code-patches)
        // to the loaded kernel image.
    }

    FORCE_INLINE auto relocate_kernel(KernelImage &image, usize const load_address) -> void {
        if (auto const status = image.relocate(load_address); status != Status::Ok) {
            panic("Failed to relocate kernel image: {}", static_cast<u32>(status));
        }
        dprintln("Relocated kernel with offset=0x{:X}", load_address);
    }

    FORCE_INLINE auto load_kernel(KernelBoot &kboot) -> void {
        kboot.load();
        dprintln("Kernel image entry point: 0x{:X}", kboot.kernel_entry_point());
        dprintln("Kernel image start address: 0x{:X}", kboot.kernel_addr());
        dprintln("Kernel image size: 0x{:X}", kboot.kernel_size());
    }

} // namespace

NO_RETURN
auto physboot_init(PhysLoadHandoff const &handoff) -> void {
    // 1. View the kernel-package BootFS physload materialized — no outer
    //    OBI re-parsing, no decompression, no scratch ownership here.
    auto const storage = KernelPackageLoader::Bytes(
        reinterpret_cast<u8 const *>(handoff.kernel_storage_base), handoff.kernel_storage_size);

    // 2. Resolve the kernel ELF inside the `kernel.name` directory
    //    (default "vmours"; file names are KernelPackage's fixed vocabulary).
    KernelPackageLoader kpackage;
    auto const package_status = kpackage.init(storage, handoff.obi_package_base,
                                              handoff.obi_package_size,
                                              handoff.boot_options->kernel_name);
    if (package_status != Status::Ok) {
        panic("Invalid kernel package: {}", static_cast<u32>(package_status));
    }

    // 3. Initialize, load, patch, and relocate the kernel image.
    KernelBoot kboot;
    kboot.init(kpackage, handoff.bootmem);

    load_kernel(kboot);

    auto &kimage = kboot.image();
    patch_kernel(kimage);

    relocate_kernel(kimage, kboot.load_address());

    // 4. Hand off to the kernel.  Never returns.
    dprintln("Handoff is at 0x{:X}", usize(global_handoff()));
    kboot.boot();

    panic("Never reach at here");
}

} // namespace ours::phys

PHYS_MODULE("physboot", ours::phys::physboot_init);
