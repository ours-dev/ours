/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_OBJECT_INFO_HPP
#define OURS_OBJECT_INFO_HPP 1

#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ours/vma.hpp>

#include <cstddef>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// What ours::object_get_info is asked about. Each topic names one record
/// layout, which the kernel writes into the caller's buffer.
///
/// The values are ABI and never change, as ObjectType's. A topic carries its
/// layout version in bits 28..31: Process is topic 3 at version 1, the layout
/// with start_time and a flags word; version 0 had three bools in their place.
///
/// HandleBasic, Process, JobProcesses, ProcessMaps and Resource are
/// answered; any other value is NotSupported. The others are added with the
/// code that answers them.
///
/// HandleBasic, Process and Resource write one record, and a buffer too
/// small for it is BufferTooSmall. JobProcesses and ProcessMaps write an
/// array: as many records as fit, with `actual` the number written and
/// `avail` the number there are, and a short buffer is not an error.
enum class InfoTopic : u32 {
    None = 0,
    HandleBasic = 2,
    Process = 3 | (1U << 28),
    /// The koids of a job's processes, a KoId each, in no promised order. A
    /// process its creator has not handed out yet -- not started, and no
    /// handle names it -- is not listed. Needs Enumerate on the job.
    JobProcesses = 9,
    /// Version 0: InfoMaps without the memory-accounting fields of later
    /// layouts, which this kernel cannot fill.
    ProcessMaps = 13,
    Resource = 18,
};

/// What object_get_info writes for InfoTopic::HandleBasic: one record, about
/// the handle asked through and the object it names.
///
/// Answered for a handle of any type, and without Inspect: it says what the
/// caller's own handle is, which its holder may always know.
struct InfoHandleBasic {
    /// The object's id: unique among every object this boot makes, never
    /// reused.
    KoId koid;

    /// The rights of the handle asked through, not of the object's other
    /// handles.
    Rights rights;

    ObjectType type;

    /// The koid of the object this one is bound to from its creation: a
    /// process's job, a thread's process, a channel end's peer (still its
    /// koid after the peer has closed). Zero for every other object.
    KoId related_koid;

    /// Zero. Explicit, as InfoProcess's padding is.
    u8 padding1[8];
};
static_assert(sizeof(InfoHandleBasic) == 32);
static_assert(alignof(InfoHandleBasic) == 8);
static_assert(offsetof(InfoHandleBasic, koid) == 0);
static_assert(offsetof(InfoHandleBasic, rights) == 8);
static_assert(offsetof(InfoHandleBasic, type) == 12);
static_assert(offsetof(InfoHandleBasic, related_koid) == 16);
static_assert(offsetof(InfoHandleBasic, padding1) == 24);

/// The state bits of an InfoProcess. Bit 2 is kept for a third, a debugger
/// being attached, left out while there is no debugger to attach.
enum class InfoProcessFlags : u32 {
    None = 0,

    /// process_start has run; start_time is valid.
    Started = 0_bit,

    /// The process has ended, by its own exit or by a kill; return_code is
    /// valid.
    Exited = 1_bit,
};
USTL_ENABLE_ENUM_BITMASK(InfoProcessFlags);

/// What object_get_info writes for InfoTopic::Process: one record.
///
/// A record crosses the ABI as bytes, so its layout is pinned below rather
/// than left to the compiler.
struct InfoProcess {
    /// What the process ended with: the value it passed to process_exit, or
    /// one of the TASK_RETCODE_* values if the kernel killed it. Valid only
    /// once flags has Exited.
    i64 return_code;

    /// When process_start ran, on the monotonic clock. Valid only once flags
    /// has Started.
    Instant start_time;

    InfoProcessFlags flags;

    /// Zero. Explicit, so that no byte the kernel copies out is left over
    /// from whatever its stack held before.
    u8 padding1[4];
};
static_assert(sizeof(InfoProcess) == 24);
static_assert(alignof(InfoProcess) == 8);
static_assert(offsetof(InfoProcess, return_code) == 0);
static_assert(offsetof(InfoProcess, start_time) == 8);
static_assert(offsetof(InfoProcess, flags) == 16);
static_assert(offsetof(InfoProcess, padding1) == 20);

/// What an InfoMaps record describes.
enum class InfoMapsType : u32 {
    None = 0,
    /// The process's address space as a whole; always the first record.
    Aspace = 1,
    /// A region (Vma) inside it.
    Vma = 2,
    /// A mapping of a VMO, or one protection range of it.
    Mapping = 3,
};

/// The part of an InfoMaps record only a Mapping has.
struct InfoMapsMapping {
    /// The range's protection: PermRead, PermWrite and PermExecute, or none
    /// where the range was unmapped inside the mapping.
    VmOptions mmu_flags;

    /// Zero.
    u8 padding1[4];

    /// The koid of the VMO mapped, 0 for one no VMO handle ever named. Two
    /// mappings of one VMO share it: a program's ELF header is in the
    /// mapping of its file at vmo_offset 0, not in its executable one.
    KoId vmo_koid;

    /// Where in the VMO the range starts.
    u64 vmo_offset;

    /// Pages of the VMO's range the VMO itself holds now; pages it would
    /// read from a parent it was cloned from, or not yet written, are not
    /// counted.
    usize committed_pages;
};
static_assert(sizeof(InfoMapsMapping) == 32);
static_assert(offsetof(InfoMapsMapping, mmu_flags) == 0);
static_assert(offsetof(InfoMapsMapping, vmo_koid) == 8);
static_assert(offsetof(InfoMapsMapping, vmo_offset) == 16);
static_assert(offsetof(InfoMapsMapping, committed_pages) == 24);

/// What object_get_info writes for InfoTopic::ProcessMaps: one record per
/// node of the process's address space, depth first in address order. The
/// address space itself comes first, at depth 0; its root region at depth
/// 1, and each region's children one deeper. A mapping whose protection
/// differs across its range is one record per range.
struct InfoMaps {
    /// The node's name, NUL-terminated: the region's for a Vma, the VMO's for
    /// a Mapping (empty when it has none), the process's for the Aspace.
    char name[MAX_NAME_LEN];
    u64 base;
    usize size;
    usize depth;
    InfoMapsType type;

    /// Zero.
    u8 padding1[4];

    /// Meaningful for a Mapping only; zero otherwise.
    union {
        InfoMapsMapping mapping;
    } u;
};
static_assert(sizeof(InfoMaps) == 96);
static_assert(alignof(InfoMaps) == 8);
static_assert(offsetof(InfoMaps, name) == 0);
static_assert(offsetof(InfoMaps, base) == 32);
static_assert(offsetof(InfoMaps, size) == 40);
static_assert(offsetof(InfoMaps, depth) == 48);
static_assert(offsetof(InfoMaps, type) == 56);
static_assert(offsetof(InfoMaps, u) == 64);

/// What object_get_info writes for InfoTopic::Resource: one record, saying
/// what the resource is a capability over.
///
/// A root resource, the one the kernel mints for a ranged kind (Irq, IoPort),
/// has base 0 and size 0 and covers the kind's whole space; any other covers
/// [base, base + size). A System resource's base is its slot, and its size
/// is 1.
struct InfoResource {
    RsrcKind kind;
    RsrcFlag flags;
    u64 base;
    u64 size;

    /// The name the kernel gave it, NUL-terminated.
    char name[MAX_NAME_LEN];
};
static_assert(sizeof(InfoResource) == 56);
static_assert(alignof(InfoResource) == 8);
static_assert(offsetof(InfoResource, kind) == 0);
static_assert(offsetof(InfoResource, flags) == 4);
static_assert(offsetof(InfoResource, base) == 8);
static_assert(offsetof(InfoResource, size) == 16);
static_assert(offsetof(InfoResource, name) == 24);
} // namespace ours

#endif // #ifndef OURS_OBJECT_INFO_HPP
