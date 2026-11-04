/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_TYPES_HPP
#define OURS_TYPES_HPP 1

#include <utility>

namespace ours {
/// A virtual address, distinct from physical addresses and byte offsets.
/// The top sixteen values are reserved for special requests, never mappings.
enum class VirtAddr : usize {
    Anywhere = ~0_usize - 1,
    Invalid = ~0_usize,
};

inline constexpr auto VIRT_ADDR_RESERVED_BASE = VirtAddr{~0_usize - 15};

/// A physical address. Zero is a real address, never an allocation request.
enum class PhysAddr : usize {
    Anywhere = ~0_usize - 1,
    Invalid = ~0_usize,
};

/// Address arithmetic keeps the address domain; a difference is a byte offset.
constexpr auto operator+(VirtAddr const base, usize const offset) -> VirtAddr {
    return VirtAddr{std::to_underlying(base) + offset};
}

constexpr auto operator-(VirtAddr const base, usize const offset) -> VirtAddr {
    return VirtAddr{std::to_underlying(base) - offset};
}

constexpr auto operator-(VirtAddr const end, VirtAddr const begin) -> usize {
    return std::to_underlying(end) - std::to_underlying(begin);
}

constexpr auto operator+=(VirtAddr &base, usize const offset) -> VirtAddr & {
    base = base + offset;
    return base;
}

constexpr auto operator-=(VirtAddr &base, usize const offset) -> VirtAddr & {
    base = base - offset;
    return base;
}

/// Address arithmetic keeps the address domain; a difference is a byte offset.
constexpr auto operator+(PhysAddr const base, usize const offset) -> PhysAddr {
    return PhysAddr{std::to_underlying(base) + offset};
}

constexpr auto operator-(PhysAddr const base, usize const offset) -> PhysAddr {
    return PhysAddr{std::to_underlying(base) - offset};
}

constexpr auto operator-(PhysAddr const end, PhysAddr const begin) -> usize {
    return std::to_underlying(end) - std::to_underlying(begin);
}

constexpr auto operator+=(PhysAddr &base, usize const offset) -> PhysAddr & {
    base = base + offset;
    return base;
}

constexpr auto operator-=(PhysAddr &base, usize const offset) -> PhysAddr & {
    base = base - offset;
    return base;
}

/// Controller-local source identity; meaningful only inside its domain.
enum class HIrqNum : u32 {
    Invalid = 0xFFFF'FFFF,
};

/// Kernel line identity. Allocated once and never reused after withdrawal.
enum class VIrqNum : u32 {
    Invalid = 0xFFFF'FFFF,
};

using CpuNum = u32;
constexpr auto INVALID_CPU_NUM = static_cast<CpuNum>(-1);

using NodeId = u32;

using PgOff = isize;

using Ticks = usize;
using Time = usize;

/// A point in time as the system-call ABI carries it: signed nanoseconds on
/// the monotonic clock. Signed so that the difference of two instants is
/// itself an instant-shaped number, and so that "a deadline in the past" is
/// representable without wrapping. It was an alias of the unsigned Time, and
/// nothing used it.
using Instant = i64;

/// A kernel object's id: unique for the life of the system, never reused.
///
/// 64 bits, because "never reused" is the whole contract and 32 bits cannot
/// keep it. Every dispatcher and every handle table draws one, a
/// channel_create() draws two, and at one id per microsecond -- a tight loop
/// of creates on one core -- 2^32 runs out in 72 minutes. The wrap then hands
/// out 0, which is KOID_INVALID and also what object::Handle::table_koid()
/// reads while a handle is in no table, and after that ids already in use: a
/// handle moved into a table that happens to share its koid with the old one
/// passes the table check it was meant to fail. At 2^64 the same loop runs
/// for half a million years.
using KoId = u64;

/// A handle as a program holds it: a number that names an entry in the
/// calling process's handle table, and means nothing anywhere else.
///
/// The word is coined: "u", for the user side that holds it, and "handle".
/// The kernel has a Handle of its own, object::Handle: the table entry a
/// Uandle names, which holds a reference to an object and the rights the
/// entry grants. A system call has both in view at once -- it is given a
/// Uandle and resolves it to an object::Handle -- so the two are never
/// spelled alike, in code or in conversation. This was HandleValue, an alias
/// of u32, until 2026-10-03.
///
/// A scoped enum rather than an alias of u32, so that a handle cannot be
/// confused with a count, a size or any other integer -- passing one where
/// the other is expected does not compile -- and cannot take part in
/// arithmetic. Only the places where a number really becomes a handle or a
/// handle a number (an argument register, in either direction, and the
/// handle table encoding or decoding a value) and where a handle is shown as
/// a number convert, and they say so. The value crosses the ABI exactly as
/// before: a scoped enum with a fixed u32 underlying type has u32's size and
/// calling convention, so it travels in the same 32-bit register.
enum class Uandle : u32 {
    /// The value no handle ever has. Closing it is a no-op that succeeds,
    /// which is what lets a caller close a slot without first asking whether
    /// it was filled.
    Invalid = 0,
};

/// The two low bits every valid handle value has set. A value without them is
/// refused before anything else is looked at, which is what keeps zero and
/// small hand-made integers from naming anything. A mask over the number, so
/// it is a u32 and is tested against std::to_underlying() of a handle.
constexpr auto HANDLE_FIXED_BITS_MASK = u32{0x3};

enum class Status : u32;
enum class Signals : u32;
enum class Rights : u32;

/// What kind of kernel object a handle names.
///
/// The values are ABI and never change, so that object_get_info can report
/// them unchanged when it exists. Only the types this kernel has a
/// dispatcher for are named. Vma, the object for a region of an address
/// space, is named after mm::VmArea (interface design 2.3).
///
/// Zero is None and never names an object -- which is also what lets it serve
/// as ustl::Rc's "no type tag" value for the checked downcast.
enum class ObjectType : u32 {
    None = 0,
    Process = 1,
    Thread = 2,
    Vmo = 3,
    Channel = 4,
    Port = 6,
    Interrupt = 9,
    Resource = 15,
    Job = 17,
    Vma = 18,
    Iommu = 23,
    Bti = 24,
    Pmt = 26,
    SuspendToken = 27,
    Pager = 28,
    Exception = 29,
    Sampler = 36,
};

/// The return code a task ends with when the kernel, not the task, ended it.
///
/// A task's return code is otherwise whatever it passed to its exit; these
/// negative values are reserved to say who ended it instead, so that a parent
/// reading the code can tell "exited with -1028" from "killed because an
/// exception went unhandled". The numbers are ABI and never change.
/// Killed through task_kill.
inline constexpr auto TASK_RETCODE_SYSCALL_KILL = i64{-1024};
/// Killed by the out-of-memory killer.
inline constexpr auto TASK_RETCODE_OOM_KILL = i64{-1025};
/// Killed by its job's policy.
inline constexpr auto TASK_RETCODE_POLICY_KILL = i64{-1026};
/// Killed by the vDSO.
inline constexpr auto TASK_RETCODE_VDSO_KILL = i64{-1027};
/// Killed because an exception went unhandled.
inline constexpr auto TASK_RETCODE_EXCEPTION_KILL = i64{-1028};
/// Killed because a process critical to its job died.
inline constexpr auto TASK_RETCODE_CRITICAL_PROCESS_KILL = i64{-1029};

} // namespace ours

/// Address constants follow the global integer-literal suffixes in prelude.hpp.
constexpr auto operator""_va(unsigned long long const value) -> ours::VirtAddr {
    return ours::VirtAddr{value};
}

constexpr auto operator""_pa(unsigned long long const value) -> ours::PhysAddr {
    return ours::PhysAddr{value};
}

#endif // #ifndef OURS_TYPES_HPP
