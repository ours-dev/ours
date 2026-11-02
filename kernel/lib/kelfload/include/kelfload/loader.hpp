/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KELFLOAD_LOADER_HPP
#define KELFLOAD_LOADER_HPP 1

#include <elfload/diagnostics.hpp>
#include <elfload/image.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vma.hpp>
#include <ustl/views/span.hpp>

namespace kelfload {
/// An ELF image laid out in a VMO and not yet mapped anywhere: what
/// Loader::load() makes and Loader::map_segments() places.
///
/// The split exists for the vDSO. It is one image every process maps, so it
/// is laid out once and that same VMO is mapped into each process; laying it
/// out per process, as the loader once did with every image it mapped, gave
/// every process a private copy of the vDSO's pages.
struct ImageVmo {
    /// Owns the VMO. Every mapping of it takes its own reference as well.
    ours::object::KernelHandle<ours::object::VmoDispatcher> vmo;

    /// The span it covers, whole pages from the lowest p_vaddr any PT_LOAD
    /// claims to the highest.
    usize span = 0;

    /// That lowest p_vaddr, page-aligned: the image address the VMO's offset 0
    /// stands for.
    ours::VirtAddr image_base = 0;

    /// e_entry, relative to image_base.
    ours::VirtAddr entry_offset = 0;
};

/// Places ELF images into one user address space.
///
/// Constructed from the address space's root VMA, because that is what it is:
/// a loader *for* that address space. Every image a process is built out of --
/// its program, the vDSO -- goes through the same one, and the object that may
/// map into the process is the thing they have in common.
///
/// It does placement only. What an image needs fixing up afterwards is
/// elfload::Binder's business, and the two are kept apart because the order
/// between them is forced: an image cannot be bound to the vDSO before the
/// vDSO has a load address, so every image is placed first and bound second.
class Loader final {
  public:
    using Bytes = ustl::views::Span<u8 const>;

    explicit Loader(ours::object::VmaDispatcher &root_vma) : m_root_vma(root_vma) {}

    /// Lays |file|'s PT_LOAD segments out in a fresh paged VMO and maps it
    /// nowhere. Static because it needs no address space -- which is what lets
    /// one layout be mapped into many.
    ///
    /// The segments land in one VMO covering one range, from the lowest
    /// p_vaddr any PT_LOAD claims to the highest, each at its own offset from
    /// the lowest. The mappings are by segment (map_segments()); the VMO is
    /// one so that an image has one origin, which is the invariant Binder
    /// relies on when it describes the image to eftl as a single load region
    /// and writes a relocation at an offset of it (elfload::Image::writer),
    /// whatever the page there is mapped as.
    ///
    /// |name| names the image in what the log says about it.
    [[nodiscard]]
    static auto load(Bytes file, char const *name, ImageVmo &out) -> ours::Status;

    /// Maps an image load() laid out at |at|, as the mappings
    /// elfload::mapping_pages() reads from |file|, the file |image| was laid
    /// out from: each segment at its own offset from |at| with the protection
    /// its p_flags ask for, its pages wholly inside PT_GNU_RELRO read-only,
    /// and the pages between segments unmapped. A segment both writable and
    /// executable, two that share a page, and RELRO anywhere but at the start
    /// of a writable segment are refused (InvArgs), with why through
    /// |diagnostics|. |extra| goes on every mapping as well (Sealed, or
    /// nothing).
    ///
    /// Every image the kernel starts is mapped this way: a program, so that
    /// none of it is writable and executable at once, and the vDSO, so that
    /// its code is the only part of it executable and the kernel's
    /// registration of it (VmaDispatcher::map()) sees exactly that segment.
    /// Each mapping is made with its final protection, before the image is
    /// bound: elfload::Binder writes relocations through the VMO
    /// (elfload::Image::writer), never through a mapping, so a page mapped
    /// read-only -- RELRO above all -- takes them all the same.
    ///
    /// A mapping's ceiling is its own protection (the matching CanMap bits):
    /// the kernel maps these images for the program, and nothing the program
    /// does later should raise them -- the vDSO above all, which must never
    /// become writable.
    ///
    /// The VMO is shared, not copied; every address space it is mapped into
    /// sees the same pages. |image| is not const because mapping takes a
    /// reference on its VMO, and ustl::Arc carries const through to the object
    /// it points at. |name| is every mapping's name, a plain C string because
    /// it is only ever a literal or a program name the caller already keeps
    /// terminated.
    ///
    /// A mapping that fails leaves the ones before it in place: a sealed one
    /// cannot be taken down, and the caller is building an address space it
    /// abandons on failure anyway.
    [[nodiscard]]
    auto map_segments(ImageVmo &image, Bytes file, ours::VirtAddr at, ours::VmOptions extra,
                      char const *name, elfload::Diagnostics const &diagnostics,
                      elfload::Image &out) const -> ours::Status;

  private:
    ours::object::VmaDispatcher &m_root_vma;
};
} // namespace kelfload

#endif // #ifndef KELFLOAD_LOADER_HPP
