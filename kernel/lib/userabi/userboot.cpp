/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The kernel side of userboot: find the first user program, build a process
/// around it, and enter it.
///
/// This is being written a step at a time, and each step is committed only
/// once it has been seen to work in QEMU. Right now it does step one: locate
/// userboot's ELF inside the kernel package. Nothing is created yet.
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/status.hpp>

#include <userabi/userboot.hpp>

namespace userabi {
using namespace ours;

namespace {
    /// The name userboot's ELF has inside the kernel package's BootFS.
    ///
    /// Not "userboot/image.elf". `kernel_elf_binary("userboot")` in
    /// //kernel/lib/userabi/userboot/BUILD.gn sets `has_patches = false`,
    /// and kernel_elf_image() installs a patch-less binary as one bare file
    /// rather than as a `<name>/` subtree -- which is why vmours, which does
    /// have patches, is "vmours/image.elf" instead. The two live in the same
    /// BootFS with different shapes, so this cannot be derived from the
    /// kernel's own name; it has to be stated.
    constexpr std::string_view USERBOOT_FILE{"userboot"};

    /// The decompressed kernel package, and the pages holding it.
    ///
    /// This is deliberately not a `ktl::Vec<u8>`. ktl's allocator is
    /// kmalloc, whose largest size class is 4 KiB, and an oversized request
    /// there answers nullptr rather than falling back to pages; Vec then
    /// constructs elements at that nullptr and page-faults inside reserve().
    /// The decompressed BootFS is megabytes -- it carries the whole kernel
    /// ELF -- so it has to come from the frame allocator and be read through
    /// the physmap. //kernel/commands/prof.cpp hit exactly this and its
    /// FdeTable carries the same note.
    struct Package {
        u8 *base = nullptr;
        usize nr_frames = 0;
    };

    /// Decompresses the kernel package into freshly allocated frames.
    ///
    /// The caller owns the frames. They are not freed here because the
    /// bytes of every file in the package -- userboot's ELF among them --
    /// are views *into* this buffer: dropping it would dangle them.
    auto materialize_kernel_package(Package &out) -> ustl::Result<phys::KernelPackage, Status> {
        // The size has to be known before the buffer can exist, and only the
        // OBI item header knows it: `memory_size` is the *decompressed*
        // length, as against the compressed length stored in the container.
        // BootObi::kernel_package_item() exists for precisely this two-step
        // (its own comment says so) -- the one-shot boot::kernel_package()
        // is for callers that already have a big enough buffer.
        auto const package = boot::boot_package();
        if (package.is_err()) {
            log::error("userboot: no boot package: {}", to_string(package.unwrap_err()));
            return ustl::err(package.unwrap_err());
        }

        auto const item = package.unwrap().kernel_package_item();
        if (item.is_err()) {
            log::error("userboot: no kernel package item: {}", to_string(item.unwrap_err()));
            return ustl::err(item.unwrap_err());
        }

        auto const bytes = static_cast<usize>(item.unwrap().memory_size());

        // The frame allocator deals in whole pages and, through get_order(),
        // in power-of-two runs of them, so round up rather than asking for a
        // byte count it cannot express.
        out.nr_frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        out.base = mm::get_frames_n<u8>(mm::GAF_KERNEL, out.nr_frames);
        if (!out.base) {
            log::error("userboot: could not allocate {} frames for the kernel package",
                       out.nr_frames);
            return ustl::err(Status::OutOfMem);
        }

        // Hands back a view over the decompressed BootFS. Everything the
        // package reports -- names, sizes, file bytes -- points into the
        // frames just allocated.
        auto result = boot::kernel_package({out.base, bytes});
        if (result.is_err()) {
            log::error("userboot: could not materialize the kernel package: {}",
                       to_string(result.unwrap_err()));
            return ustl::err(result.unwrap_err());
        }

        return ustl::ok(result.unwrap());
    }
} // namespace

auto userboot_init() -> void {
    Package package_frames;
    auto const package = materialize_kernel_package(package_frames);
    if (package.is_err()) {
        return;
    }

    // Listing the whole package rather than going straight for the one file
    // is worth the few lines: the names come from the GN packaging rules,
    // and a mismatch between what the build installs and what this looks up
    // is otherwise invisible -- find() just answers NotFound and says
    // nothing about what *is* there.
    auto const &bootfs = package.unwrap().bootfs();
    log::info("userboot: kernel package holds {} files", bootfs.file_count());
    for (auto index = u32(0); index < bootfs.file_count(); ++index) {
        log::info("userboot:   {} ({} bytes)", bootfs.file_name(index),
                  bootfs.file_data(index).size());
    }

    auto const image = package.unwrap().file(USERBOOT_FILE);
    if (image.is_err()) {
        log::error("userboot: '{}' is not in the kernel package: {}", USERBOOT_FILE,
                   to_string(image.unwrap_err()));
        return;
    }

    log::info("userboot: found '{}', {} bytes of ELF", USERBOOT_FILE, image.unwrap().size());

    // Next: turn those bytes into a VMO, build a ProcessDispatcher with a
    // user address space, map the ELF's segments into it, and start a thread
    // at its entry point. None of that exists yet.
}
} // namespace userabi
