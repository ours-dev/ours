/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_TYPES_HPP
#define OURS_TYPES_HPP 1

namespace ours {
using VirtAddr = usize;
using PhysAddr = usize;

using HIrqNum = u32;
using VIrqNum = u32;

using CpuNum = u32;
constexpr auto INVALID_CPU_NUM = static_cast<CpuNum>(-1);

using NnId = u32;

using PgOff = isize;

using Ticks = usize;
using Time = usize;

/// A point in time as the system-call ABI carries it: signed nanoseconds on
/// the monotonic clock, as Zircon's zx_instant_mono_t. Signed so that the
/// difference of two instants is itself an instant-shaped number, and so that
/// "a deadline in the past" is representable without wrapping. It was an
/// alias of the unsigned Time, and nothing used it.
using Instant = i64;

using TicksSteady = Ticks;
using InstantSteady = Instant;
using DurationTicksSteady = usize;

using TicksSystem = Ticks;
using InstantSystem = Instant;
using DurationTicksSystem = usize;

/// A kernel object's id: unique for the life of the system, never reused.
///
/// 64 bits, as Zircon's zx_koid_t, because "never reused" is the whole
/// contract and 32 bits cannot keep it. Every dispatcher and every handle
/// table draws one, a channel_create() draws two, and at one id per
/// microsecond -- a tight loop of creates on one core -- 2^32 runs out in
/// 72 minutes. The wrap then hands out 0, which is KOID_INVALID and also
/// what Handle::table_koid() reads while a handle is in no table, and after
/// that ids already in use: a handle moved into a table that happens to share
/// its koid with the old one passes the table check it was meant to fail.
/// At 2^64 the same loop runs for half a million years.
using KoId = u64;

/// A handle as a program holds it: a number that names an entry in the
/// calling process's handle table, and means nothing anywhere else.
///
/// Called HandleValue rather than Handle, which it used to be, because the
/// system-call layer has both in view at once: this number, and the
/// object::Handle it resolves to. Two types spelled `Handle` in one function
/// is a misreading waiting to happen.
using HandleValue = u32;

/// The value no handle ever has. Closing it is a no-op that succeeds, which
/// is what lets a caller close a slot without first asking whether it was
/// filled (Zircon ZX_HANDLE_INVALID).
constexpr auto HANDLE_INVALID = HandleValue{0};

/// The two low bits every valid handle value has set. A value without them is
/// refused before anything else is looked at, which is what keeps zero and
/// small hand-made integers from naming anything (Zircon
/// ZX_HANDLE_FIXED_BITS_MASK).
constexpr auto HANDLE_FIXED_BITS_MASK = HandleValue{0x3};

enum class Status : u32;
enum class Signals : u32;
enum class Rights : u32;

/// What kind of kernel object a handle names.
///
/// The values are Zircon's ZX_OBJ_TYPE_* (zircon/types.h), so that
/// object_get_info can report them unchanged when it exists. Only the types
/// this kernel has a dispatcher for are named. Vma is Zircon's VMAR, named
/// after mm::VmArea (interface design 2.3).
///
/// Zero is None and never names an object -- which is also what lets it serve
/// as ustl::Rc's "no type tag" value for the checked downcast.
enum class ObjectType : u32 {
    None = 0,
    Process = 1,
    Thread = 2,
    Vmo = 3,
    Channel = 4,
    Resource = 15,
    Job = 17,
    Vma = 18,
    Pager = 28,
};

enum class Clock : u32 {
    System,
    Steady,
};

/// The return code a task ends with when the kernel, not the task, ended it.
///
/// A task's return code is otherwise whatever it passed to its exit; these
/// negative values are reserved to say who ended it instead, so that a parent
/// reading the code can tell "exited with -1028" from "killed because an
/// exception went unhandled". The numbers are Zircon's (ZX_TASK_RETCODE_*),
/// kept identical because they are ABI and there is no reason to differ.
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

#endif // #ifndef OURS_TYPES_HPP
