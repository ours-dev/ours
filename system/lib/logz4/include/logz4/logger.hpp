/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_LOGGER_HPP
#define LOGZ4_LOGGER_HPP 1

#include <ours/types.hpp>

namespace logz4 {

/// A log sink. Implementations own their own serialization: logz4 hands
/// each fully assembled line over with a single write() call, and the sink
/// decides what keeps whole lines from interleaving when several CPUs log
/// at once (the kernel's serial console takes an irq-saving spinlock).
class Logger {
  public:
    virtual ~Logger() noexcept = default;

    virtual auto write(char const *s, usize n) -> void = 0;

    /// True only for the pre-init placeholder. An Error-severity line
    /// arriving while the null logger is installed is routed to
    /// emergency_write() instead, so early panics still reach a boot port.
    [[nodiscard]]
    virtual auto is_null() const -> bool {
        return false;
    }
};

} // namespace logz4

#endif // LOGZ4_LOGGER_HPP
