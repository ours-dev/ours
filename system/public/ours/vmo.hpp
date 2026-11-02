/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VMO_HPP
#define OURS_VMO_HPP 1

// The VMO calls and their option words, one domain in one file: a program
// that makes, reads, writes and clones memory objects includes this and
// nothing else of the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// vmo_create's options. Only None is accepted: Resizable is recognised and
/// answered NotSupported (it is deferred past M1), and any other bit is
/// InvArgs. Resizable already has its ABI value, so that a program asking for
/// it now asks the same question once it is built.
enum class VmoOptions : u32 {
    None = 0,
    Resizable = 1U << 1,
};

/// vmo_create_child's options: which kind of child, and how it may be used.
/// Slice and the three snapshots are built, each alone; every other
/// bit named here is recognised and answered NotSupported, and any bit not
/// named here is InvArgs. The values are ABI and never change.
///
/// The three snapshots are named for how much of the parent they freeze.
///
/// Declared by what they are, not by value: first the kind of child, of which a
/// request names one, then what may be added to it. The ABI handed the values
/// out one at a time as options were added to it, so they do not run in
/// order, and bit 1 belonged to an option it has retired: it is kept out of
/// use, never given to a new one.
enum class VmoChildOptions : u32 {
    None = 0,

    // The kind of child.

    /// A copy-on-write copy of everything as it was at the clone: neither the
    /// child nor the parent ever sees the other's later writes. Built:
    /// mm::VmObjectPaged::create_clone().
    SnapshotFull = 0_bit,

    /// Only a page the child has written is certainly its own; a page it has
    /// not written may still show the parent's later writes. What a loader asks
    /// for a data segment. Built as SnapshotFull, which gives more than this
    /// asks: the child never sees the parent's later writes.
    SnapshotOnWrite = 1_bit,

    /// The pages the parent has modified are frozen; the ones still as a pager
    /// supplied them are not, and keep following the pager. For fork() of a
    /// mapping of a pager-backed file. Built as SnapshotFull, which is what
    /// this asks when there is no pager: then no page is a pager's, so every
    /// page is frozen.
    SnapshotModified = 2_bit,

    /// A window onto the parent's own pages, no copy: a write through either is
    /// seen through both. Its rights are the parent handle's, never more.
    /// Built: mm::VmObjectPaged::create_slice().
    Slice = 3_bit,

    /// The whole parent again as another VMO: the same pages and the same size,
    /// so offset and size must both be 0. NotSupported.
    Reference = 4_bit,

    // What may be added to it.

    /// The child can be resized later. NotSupported: a child is made at its
    /// final size.
    Resizable = 5_bit,

    /// The child's handle has no Write, which a snapshot otherwise gets.
    /// NotSupported.
    NoWrite = 6_bit,
};
USTL_ENABLE_ENUM_BITMASK(VmoChildOptions);

// The VMO calls, as a program calls them: generated from
// //system/public/ours/syscalls/vmo.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/vmo.inc>
} // namespace ours

#endif // OURS_VMO_HPP
