/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_LOADER_HPP
#define ELFLOAD_LOADER_HPP 1

#include <elfload/image.hpp>

#include <ours/object/vma_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace elfload {
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

    /// Lays |file|'s PT_LOAD segments out in fresh frames and maps them at
    /// |where| with |flags|.
    ///
    /// The segments land in one contiguous run of frames, from the lowest
    /// p_vaddr any PT_LOAD claims to the highest. That is a simplification --
    /// a real loader honours each segment's own permissions and the gaps
    /// between them -- and it is also the invariant Binder relies on when it
    /// describes the image to eftl as a single load region.
    ///
    /// |name| is a plain C string because it becomes the mapping's name and is
    /// only ever a literal or a program name the caller already keeps
    /// terminated.
    [[nodiscard]]
    auto map(Bytes file, ours::mm::VmPlacement where, ours::mm::MmuFlags flags, char const *name,
             Image &out) const -> ours::Status;

  private:
    ours::object::VmaDispatcher &m_root_vma;
};
} // namespace elfload

#endif // #ifndef ELFLOAD_LOADER_HPP
