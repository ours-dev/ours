/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_LOADER_HPP
#define ELFLOAD_LOADER_HPP 1

#include <elfload/image.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/vmar_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace elfload {
/// An ELF image laid out in a VMO and not yet mapped anywhere: what
/// Loader::load() makes and Loader::map() places.
///
/// The split exists for the vDSO. It is one image every process maps, and
/// Zircon lays it out once (VDso::Create(), zircon/kernel/lib/userabi/vdso.cc)
/// and maps that same VMO into each process; laying it out per process, as
/// map() alone does, gave every process a private copy of the vDSO's pages.
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
/// Constructed from the address space's root VMAR, because that is what it is:
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

    explicit Loader(ours::object::VmarDispatcher &root_vmar) : m_root_vmar(root_vmar) {}

    /// Lays |file|'s PT_LOAD segments out in a fresh paged VMO and maps it at
    /// the address |at| with the protection |perms| (VmOptions::Perm* bits
    /// only).
    ///
    /// The segments land in one VMO covering one range, from the lowest
    /// p_vaddr any PT_LOAD claims to the highest. That is a simplification --
    /// a real loader honours each segment's own permissions and the gaps
    /// between them -- and it is also the invariant Binder relies on when it
    /// describes the image to eftl as a single load region.
    ///
    /// |name| is a plain C string because it becomes the mapping's name and is
    /// only ever a literal or a program name the caller already keeps
    /// terminated.
    [[nodiscard]]
    auto map(Bytes file, ours::VirtAddr at, ours::VmOptions perms, char const *name,
             Image &out) const -> ours::Status;

    /// The first half of the above: lays |file|'s PT_LOAD segments out in a
    /// fresh paged VMO and maps it nowhere. Static because it needs no address
    /// space -- which is what lets one layout be mapped into many.
    [[nodiscard]]
    static auto load(Bytes file, char const *name, ImageVmo &out) -> ours::Status;

    /// The second half: maps an image load() laid out at |at| with |perms|.
    /// The VMO is shared, not copied; every address space it is mapped into
    /// sees the same pages.
    ///
    /// The mapping's ceiling is |perms| itself (the matching CanMap bits):
    /// the kernel maps these images for the program, and nothing the program
    /// does later should raise them -- the vDSO above all, which must never
    /// become writable.
    ///
    /// |image| is not const because mapping takes a reference on its VMO, and
    /// ustl::Arc carries const through to the object it points at.
    [[nodiscard]]
    auto map(ImageVmo &image, ours::VirtAddr at, ours::VmOptions perms, char const *name,
             Image &out) const -> ours::Status;

  private:
    ours::object::VmarDispatcher &m_root_vmar;
};
} // namespace elfload

#endif // #ifndef ELFLOAD_LOADER_HPP
