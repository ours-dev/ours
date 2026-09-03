/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_LOCKDEP_HPP
#define OURS_LOCKDEP_HPP 1

#include <lockdep/guard.hpp>
#include <lockdep/state_saver.hpp>

namespace ours {
/// The option tags a Guard takes, re-exported so call sites can write
/// `Guard<SpinLock, IrqSave>` unqualified.
///
/// These were two freshly-declared empty structs, `struct IrqSave {};` and
/// `struct PreemptDisable {};`, unrelated to the lockdep tags of the same
/// name. Since unqualified lookup inside `namespace ours` finds these first,
/// every `Guard<SpinLock, IrqSave>` in the kernel was parameterised on
/// ours::IrqSave -- a type the state-saver machinery has never heard of, and
/// which lockdep::StateSaver cannot map to any saver. Guard also ignored its
/// options entirely at the time, so the mismatch stayed invisible: the guards
/// took their locks and left interrupts enabled, and the debug kernel only
/// gave it away much later by stopping on
/// `DEBUG_ASSERT(arch::irqs_disabled())` inside
/// Scheduler::reschedule_common().
using lockdep::IrqSave;
using lockdep::IrqSaveAndPreemptDisable;
using lockdep::PreemptDisable;

using lockdep::Guard;
using lockdep::ScopedGuard;

} // namespace ours

#endif // #ifndef OURS_LOCKDEP_HPP