/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_OBJECT_INFO_HPP
#define OURS_OBJECT_INFO_HPP 1

#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>

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
/// HandleBasic, Process and Resource are answered; any other value is
/// NotSupported. The others are added with the code that answers them.
enum class InfoTopic : u32 {
    None = 0,
    HandleBasic = 2,
    Process = 3 | (1U << 28),
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
