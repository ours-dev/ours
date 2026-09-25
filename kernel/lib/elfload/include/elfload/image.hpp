/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_IMAGE_HPP
#define ELFLOAD_IMAGE_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace elfload {
/// Where writes into a placed image go: |write| copies |size| bytes from
/// |bytes| to byte |offset| of the image, counted from its lowest PT_LOAD
/// address, and says whether it could.
///
/// A function pointer and its argument, not a virtual interface and not a
/// template parameter: the Binder is one piece of code whatever the image is
/// backed by, and the backing is chosen at run time by whoever loaded it.
struct ImageWriter {
    auto (*write)(void *context, usize offset, void const *bytes, usize size)
        -> ours::Status = nullptr;
    void *context = nullptr;
};

/// One ELF laid into memory: what a Loader produces and a Binder fixes up.
///
/// The two halves of loading share exactly this, and nothing else. A Loader
/// needs an address space and the object layer to produce it; a Binder needs
/// none of that -- it takes the image as a fact and writes into it -- which is
/// what lets the binding half be tested without a kernel around it.
struct Image {
    /// Where the image starts in the address space it was mapped into.
    ours::VirtAddr base = 0;

    /// How much address space it covers, from the lowest p_vaddr any PT_LOAD
    /// claims to the highest, rounded out to whole pages.
    usize span = 0;

    /// e_entry, moved to wherever the image landed. The first instruction of a
    /// program; meaningless for an image nothing jumps to, such as the vDSO.
    ours::VirtAddr entry = 0;

    /// How to write into the same bytes, for the relocations.
    ///
    /// Relocations have to be written before the image runs, and the writer is
    /// not in the address space the image was mapped into -- the kernel maps a
    /// user program and then fixes it up from kernel context, where those user
    /// addresses mean something else entirely.
    ///
    /// This used to be a pointer to the bytes, which only worked while the
    /// loader put an image in one physically contiguous run of frames. It now
    /// backs an image with a paged VMO, whose pages are wherever the allocator
    /// found them, and the user-space launcher to come will not map the image
    /// into itself at all but write through vmo_write
    /// (docs/design/260924/userspace-filesystem.md, 4.6). "Put these bytes at
    /// this offset of the image" is what both can offer.
    ImageWriter writer;

    /// The lowest p_vaddr the ELF claims, which is the origin every address in
    /// its headers and dynamic tables is relative to. `base - image_base` is
    /// the load bias.
    ours::VirtAddr image_base = 0;
};
} // namespace elfload

#endif // #ifndef ELFLOAD_IMAGE_HPP
