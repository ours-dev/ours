/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYSCALL_HPP
#define OURS_SYSCALL_HPP 1

// The constants a program and the kernel have to agree on about system calls:
// limits, sentinels and option words, each named once so that both sides
// compile against the same value.
//
// What is not here is the call numbers. They used to be, as OURS_SYS_* macros
// with a count beside them, on the grounds that "a user program puts one of
// them in the syscall-number register". No user program does: the only
// `syscall` instruction in a user address space is in the vDSO, and the kernel
// refuses a trap from anywhere else. A program names a call by its vDSO symbol
// (<ours/vdso.hpp>), so the numbers are a private agreement between the vDSO
// and the kernel's dispatch table (design D8). Both are now generated from
// //system/public/ours/syscalls.gni, into a header only those two can see
// (//system/public:syscalls.private). Publishing the numbers would only have
// invited the one thing the vDSO exists to prevent.

#include <ours/types.hpp>

#include <limits>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// A deadline that never arrives: wait until the condition holds, however
/// long that takes (Zircon ZX_TIME_INFINITE).
///
/// Its own constant rather than the kernel's INFINITE_TIME, which is a chrono
/// TimePoint the kernel's timers are written against. This one is the plain
/// number a program passes in a register.
inline constexpr auto INSTANT_INFINITE = std::numeric_limits<Instant>::max();

/// The most bytes one channel message may carry.
///
/// 4 KiB, which is the largest single kmalloc: a message is copied into one
/// kernel allocation before the channel's lock is taken, and that allocation
/// does not go through vmalloc, whose 256 regions are shared by the whole
/// system and whose vfree has no TLB shootdown yet. Zircon allows 64 KiB;
/// raising this later is compatible, lowering it would not be.
inline constexpr auto CHANNEL_MAX_MSG_BYTES = u32{4096};

/// The most handles one channel message may carry (Zircon's number).
inline constexpr auto CHANNEL_MAX_MSG_HANDLES = u32{64};

/// The most live handles one process may have created, wherever they are now:
/// in its own table, in another process's, or queued in a message on the
/// way. Creating one past it answers NoResource; closing one gives the room
/// back.
///
/// Counted against the creator, not the holder, so that sending handles
/// away does not make room for more: a process that makes channels and
/// writes their ends into a channel nobody reads would otherwise fill the
/// system-wide arena from a table that always looks empty. A quarter of
/// today's arena, so that no one process can exhaust it alone; it grows when
/// the arena does.
inline constexpr auto HANDLE_MAX_PER_PROCESS = u32{1024};

/// The most bytes one debug_write call will copy.
///
/// A ceiling on one call, so that a program cannot hold a CPU inside the
/// kernel for as long as it likes by asking for a gigabyte of output. The
/// number is arbitrary; what is not arbitrary is that there is one, and that
/// both sides know the same one. It lives here rather than in the handler
/// because the caller has to split its text to fit -- a limit only the kernel
/// knows is a limit the caller discovers as Status::OutOfRange.
inline constexpr auto MAX_DEBUG_WRITE = usize{256};

/// The longest program name process_spawn_boot accepts, in bytes, not
/// counting a terminator (none is passed). The kernel keeps names in 64-byte
/// buffers, as Zircon's kernel.select.userboot does.
inline constexpr auto PROCESS_BOOT_NAME_MAX = usize{63};

/// Options for the channel calls. None is the only value any of them accepts
/// today; anything else answers InvArgs rather than being ignored, so that a
/// program cannot come to depend on a bit that does nothing. They are
/// separate types, one per call, because the bits each will grow are
/// unrelated (Zircon's ZX_CHANNEL_WRITE_USE_IOVEC and ZX_CHANNEL_READ_MAY_DISCARD
/// share no meaning), and a type per call makes passing one to the other a
/// compile error.
enum class ChannelCreateOptions : u32 {
    None = 0,
};

enum class ChannelWriteOptions : u32 {
    None = 0,
};

/// process_create's options. None is the only value it accepts today; anything
/// else is InvArgs. Zircon's are also empty (the argument is reserved).
enum class ProcessCreateOptions : u32 {
    None = 0,
};

/// thread_create's options, on the same terms.
enum class ThreadCreateOptions : u32 {
    None = 0,
};

enum class ChannelReadOptions : u32 {
    None = 0,
};

/// vmo_create's options. Only None is accepted: Resizable is recognised and
/// answered NotSupported (it is deferred past M1), and any other bit is
/// InvArgs. Resizable carries Zircon's ZX_VMO_RESIZABLE value so that a
/// request ported from Zircon asks the same question.
enum class VmoOptions : u32 {
    None = 0,
    Resizable = 1U << 1,
};

/// vma_map's options (and, later, vma_allocate's and vma_protect's): what
/// the mapping may do, where it goes, and how it is populated. The values are
/// Zircon's ZX_VM_*, so a request ported from Zircon asks the same question.
///
///   Perm*       The protection the pages get.
///   Specific    At vma_offset from the region's start, not wherever fits.
///   CanMap*     The ceiling a later protect() is held to. Set by the kernel
///               from the handles' rights; a program may not pass them.
///   MapRange    Populate the whole range now rather than on first touch.
///   Sealed      Make the mapping immutable from the moment it exists: it can
///               never be unmapped, have its protection changed, or be
///               destroyed with its region. Irreversible. Ours' own, with no
///               ZX_VM_* counterpart, so it sits at bit 20, away from the bits
///               Zircon uses. The vDSO's VMO can only be mapped this way.
/// The rest are named so that a program asking for them gets NotSupported,
/// not InvArgs: they are Zircon's, deferred here.
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

/// vmo_create_child's options: which kind of child, and how it may be used.
/// Slice and the three snapshots are built, each alone; every other
/// bit named here is recognised and answered NotSupported, and any bit not
/// named here is InvArgs. The values are Zircon's ZX_VMO_CHILD_*, so a request
/// ported from Zircon asks the same question.
///
/// The three snapshots are named for how much of the parent they freeze, as
/// Zircon's kernel names them internally (SnapshotType::Full, OnWrite,
/// Modified), rather than as its syscall header does.
///
/// Declared by what they are, not by value: first the kind of child, of which a
/// request names one, then what may be added to it. The values stay Zircon's,
/// which were handed out one at a time as the kinds were added, so they do not
/// run in order and bit 1 is not used.
enum class VmoChildOptions : u32 {
    None = 0,

    // The kind of child.

    /// A copy-on-write copy of everything as it was at the clone: neither the
    /// child nor the parent ever sees the other's later writes. Built:
    /// mm::VmObjectPaged::create_clone().
    SnapshotFull = 1U << 0,

    /// Only a page the child has written is certainly its own; a page it has
    /// not written may still show the parent's later writes. What a loader asks
    /// for a data segment (Zircon's elfldltl). Built as SnapshotFull, which
    /// gives more than this asks: the child never sees the parent's later
    /// writes.
    SnapshotOnWrite = 1U << 4,

    /// The pages the parent has modified are frozen; the ones still as a pager
    /// supplied them are not, and keep following the pager. For fork() of a
    /// mapping of a pager-backed file. Built as SnapshotFull, which is what
    /// this asks when there is no pager: then no page is a pager's, so every
    /// page is frozen.
    SnapshotModified = 1U << 7,

    /// A window onto the parent's own pages, no copy: a write through either is
    /// seen through both. Its rights are the parent handle's, never more.
    /// Built: mm::VmObjectPaged::create_child_slice().
    Slice = 1U << 3,

    /// The whole parent again as another VMO: the same pages and the same size,
    /// so offset and size must both be 0. NotSupported.
    Reference = 1U << 6,

    // What may be added to it.

    /// The child can be resized later. NotSupported: a child is made at its
    /// final size.
    Resizable = 1U << 2,

    /// The child's handle has no Write, which a snapshot otherwise gets.
    /// NotSupported.
    NoWrite = 1U << 5,
};
} // namespace ours

#endif // OURS_SYSCALL_HPP
