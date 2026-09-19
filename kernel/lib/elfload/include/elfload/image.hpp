/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_IMAGE_HPP
#define ELFLOAD_IMAGE_HPP 1

#include <ours/types.hpp>

namespace elfload {
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

    /// The loader's own view of the same bytes.
    ///
    /// Relocations have to be written before the image runs, and the writer is
    /// not in the address space the image was mapped into -- the kernel maps a
    /// user program and then fixes it up from kernel context, where those user
    /// addresses mean something else entirely. This pointer is the same memory
    /// reached the way the writer can reach it.
    u8 *view = nullptr;

    /// The lowest p_vaddr the ELF claims, which is the origin every address in
    /// its headers and dynamic tables is relative to. `base - image_base` is
    /// the load bias.
    ours::VirtAddr image_base = 0;
};
} // namespace elfload

#endif // #ifndef ELFLOAD_IMAGE_HPP
