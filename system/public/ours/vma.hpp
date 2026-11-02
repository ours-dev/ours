/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VMA_HPP
#define OURS_VMA_HPP 1

// The address-region (Vma) calls and their option words, one domain in one
// file: a program that reserves and maps address space includes this and
// nothing else of the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// vma_map's and vma_allocate's options (and, later, vma_protect's): what
/// a mapping or sub-region may do, where it goes, and how it is populated.
/// The values are ABI and never change.
///
///   Perm*       The protection the pages get. vma_map only.
///   Specific    At the offset from the region's start, not wherever fits;
///               only into a region that has CanMapSpecific (AccessDenied
///               otherwise, asked before whether the range lies inside the
///               region). A process's root region has it.
///   CanMap*     On vma_map, none may be passed (InvArgs): the kernel sets
///               CanMapRead, CanMapWrite and CanMapExecute from the handles'
///               rights, as the ceiling a later protect() is held to. On
///               vma_allocate, CanMapRead, CanMapWrite and
///               CanMapExecute say what mappings in the new sub-region may
///               allow, each needing the matching right on the parent's
///               handle, and none at all makes the sub-region a guard;
///               CanMapSpecific lets the sub-region take Specific placements,
///               whether or not the parent can.
///   MapRange    Populate the whole range now rather than on first touch.
///   Sealed      Make the mapping immutable from the moment it exists: no
///               call can unmap it, change its protection, or destroy it with
///               its region (AccessDenied, and nothing else in the range is
///               touched). Irreversible. Only the address space's teardown,
///               when the process dies, takes it down. It sits at bit 20,
///               clear of the bits the other options use and are kept for.
///               The vDSO's code can only be mapped executable this way.
/// The rest are named so that a program asking for them gets NotSupported,
/// not InvArgs: they are deferred here. Bit 15 (faulting past the end of a
/// stream's size) and an alignment field in bits 24 to 31 are kept for later
/// options and not named yet, so they are still InvArgs.
enum class VmOptions : u32 {
    None = 0,
    PermRead = 1U << 0,
    PermWrite = 1U << 1,
    PermExecute = 1U << 2,
    Compact = 1U << 3,
    Specific = 1U << 4,
    SpecificOverwrite = 1U << 5,
    CanMapSpecific = 1U << 6,
    CanMapRead = 1U << 7,
    CanMapWrite = 1U << 8,
    CanMapExecute = 1U << 9,
    MapRange = 1U << 10,
    RequireNonResizable = 1U << 11,
    AllowFaults = 1U << 12,
    OffsetIsUpperLimit = 1U << 13,
    PermReadIfXomUnsupported = 1U << 14,
    Sealed = 1U << 20,
};
USTL_ENABLE_ENUM_BITMASK(VmOptions);

// The Vma calls, as a program calls them: generated from
// //system/public/ours/syscalls/vma.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/vma.inc>
} // namespace ours

#endif // OURS_VMA_HPP
