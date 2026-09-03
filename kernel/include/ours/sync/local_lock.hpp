/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_LOCAL_LOCK_HPP
#define OURS_SYNC_LOCAL_LOCK_HPP 1

#include <arch/auto_interrupt_disabler.hpp>
#include <lockdep/state_saver.hpp>
#include <ours/sync/config.hpp>

namespace ours::sync {
class TA_CAPABILITY("LocalLock") LocalLock {
    using Self = LocalLock;

  public:
    TA_ACQUIRE() auto acquire() -> void;
    TA_RELEASE() auto release() -> void;

  private:
};

/// mm::ObjectCache guards its per-CPU free lists with `Guard<LocalLock,
/// IrqSave>`, and that combination has to resolve to a saver the same way
/// SpinLock's does -- see the note in spin_lock.hpp. Without this declaration
/// the Guard has no state saver to name and does not compile.
LOCKDEP_STATE_SAVER(LocalLock, lockdep::SavedStateOption::IrqSave,
                    ::arch::AutoInterruptDisabler);
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_LOCAL_LOCK_HPP