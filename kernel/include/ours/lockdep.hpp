/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_LOCKDEP_HPP
#define OURS_LOCKDEP_HPP 1

#include <lockdep/guard.hpp>
#include <lockdep/state_saver.hpp>

namespace ours {
    struct IrqSave {};
    struct PreemptDisable {};

    using lockdep::Guard;
    using lockdep::ScopedGuard;

} // namespace ours

#endif // #ifndef OURS_LOCKDEP_HPP