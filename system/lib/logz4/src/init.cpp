/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/clock.hpp>
#include <logz4/log.hpp>

#include <atomic>

#include <ours/compiler.hpp>

namespace logz4 {
namespace {
    /// The logger installed before anyone calls init(): accepts and drops
    /// every line. get_global_logger() therefore never returns null, and
    /// pre-init log calls are silent instead of fatal.
    class NullLogger final: public Logger {
      public:
        auto write(char const *, usize) -> void override {}

        [[nodiscard]]
        auto is_null() const -> bool override {
            return true;
        }
    };

    NullLogger null_logger;

    /// Published with release stores and read with acquire loads: a logger
    /// constructed on one CPU must be fully visible to the next log call on
    /// any other. Constant-initialized, so it is ready before any dynamic
    /// initializer could conceivably log.
    std::atomic<Logger *> global_logger{&null_logger};

    Time (*registered_clock)() = nullptr;
    u32 (*registered_cpu)() = nullptr;

    /// Relaxed, because it publishes nothing but itself. The CPU that sets it
    /// reads it back in its own later writes, which program order covers. A
    /// CPU that sees it late only waits for the lock in the ordinary way for
    /// a little longer.
    std::atomic<bool> panic_mode{false};
} // namespace

/// Default: drop. Architectures with a known-good boot port define this
/// with strong linkage so pre-logger panics still reach the serial port.
WEAK_LINK
auto emergency_write(char const *, usize n) -> void {
    static_cast<void>(n);
}

auto enter_panic_mode() -> void {
    panic_mode.store(true, std::memory_order_relaxed);
}

auto in_panic_mode() -> bool {
    return panic_mode.load(std::memory_order_relaxed);
}

auto init(Logger *logger) -> void {
    if (logger == nullptr) {
        return;
    }
    set_global_logger(*logger);
}

auto set_global_logger(Logger &logger) -> void {
    global_logger.store(&logger, std::memory_order_release);
}

auto get_global_logger() -> Logger & {
    return *global_logger.load(std::memory_order_acquire);
}

auto set_clock(Time (*read)()) -> void {
    registered_clock = read;
}

auto clock() -> Time (*)() {
    return registered_clock;
}

auto set_cpu(u32 (*read)()) -> void {
    registered_cpu = read;
}

auto cpu() -> u32 (*)() {
    return registered_cpu;
}
} // namespace logz4
