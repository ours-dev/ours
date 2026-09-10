#include <ccrt/runtime.hpp>

#include <ours/compiler.hpp>

namespace ccrt {
namespace {
    using Byte = unsigned char;

    constexpr Byte GUARD_UNINITIALIZED = 0;
    constexpr Byte GUARD_INITIALIZED = 1;
    constexpr Byte GUARD_UNLOCKED = 0;
    constexpr Byte GUARD_LOCKED = 1;

    auto load_acquire(Byte const *value) noexcept -> Byte {
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    auto store_release(Byte *value, Byte desired) noexcept -> void {
        __atomic_store_n(value, desired, __ATOMIC_RELEASE);
    }

    auto compare_exchange_acquire(Byte *value, Byte &expected, Byte desired) noexcept -> bool {
        return __atomic_compare_exchange_n(value, &expected, desired, false, __ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED);
    }

    auto exchange_acquire(Byte *value, Byte desired) noexcept -> Byte {
        return __atomic_exchange_n(value, desired, __ATOMIC_ACQUIRE);
    }

    constinit Guard INITIALIZERS_GUARD{};

    constinit Registry REGISTRY{};

    extern "C" Initializer const INITIALIZERS_START[] LINK_NAME("__ccrt_init_start");
    extern "C" Initializer const INITIALIZERS_END[] LINK_NAME("__ccrt_init_end");
} // namespace

auto Registry::instance() noexcept -> Registry & {
    return REGISTRY;
}

namespace {
    /// Raw COM1 write for boot-step markers; see the x86 arch handoff. Used
    /// to stamp each global initializer's index so a ctor that faults before
    /// the console exists is still identifiable from the serial stream.
    auto com1_put(Byte const c) noexcept -> void {
        unsigned short const port = 0x3F8;
        asm volatile("out %0, %1" : : "a"(c), "d"(port));
    }
} // namespace

auto init_global_static_objects() -> void {
    if (!INITIALIZERS_GUARD.acquire()) {
        return;
    }
    Byte index = 0;
    for (auto initializer = INITIALIZERS_START; initializer != INITIALIZERS_END; ++initializer) {
        com1_put(index);
        (*initializer)();
        ++index;
    }
    INITIALIZERS_GUARD.release();
}

auto Guard::acquire() noexcept -> bool {
    auto *const initialized = &m_state[0];
    auto *const lock = &m_state[1];
    if (load_acquire(initialized) != GUARD_UNINITIALIZED) {
        return false;
    }

    while (true) {
        if (load_acquire(initialized) != GUARD_UNINITIALIZED) {
            return false;
        }

        auto expected = GUARD_UNLOCKED;
        if (compare_exchange_acquire(lock, expected, GUARD_LOCKED)) {
            if (load_acquire(initialized) != GUARD_UNINITIALIZED) {
                store_release(lock, GUARD_UNLOCKED);
                return false;
            }
            return true;
        }

        while (load_acquire(lock) != GUARD_UNLOCKED &&
               load_acquire(initialized) == GUARD_UNINITIALIZED) {
        }
    }
}

auto Guard::release() noexcept -> void {
    store_release(&m_state[0], GUARD_INITIALIZED);
    store_release(&m_state[1], GUARD_UNLOCKED);
}

auto Guard::abort() noexcept -> void {
    store_release(&m_state[0], GUARD_UNINITIALIZED);
    store_release(&m_state[1], GUARD_UNLOCKED);
}

auto Registry::lock() noexcept -> void {
    while (exchange_acquire(&m_lock, GUARD_LOCKED) != GUARD_UNLOCKED) {
    }
}

auto Registry::unlock() noexcept -> void {
    store_release(&m_lock, GUARD_UNLOCKED);
}

auto Registry::compact() noexcept -> void {
    auto write_index = 0U;
    for (auto read_index = 0U; read_index != m_record_count; ++read_index) {
        auto const &record = m_records[read_index];
        if (!record.m_active) {
            continue;
        }
        if (write_index != read_index) {
            m_records[write_index] = record;
        }
        ++write_index;
    }
    m_record_count = write_index;
}

auto Registry::matches(Record const &record, Scope scope) const noexcept -> bool {
    if (!record.m_active) {
        return false;
    }
    if (scope.m_identity == nullptr) {
        return true;
    }
    return record.m_scope.m_identity == scope.m_identity;
}

auto Registry::register_callback(Callback callback, void *argument, Scope scope,
                                 CallbackKind kind) noexcept -> bool {
    lock();
    compact();
    if (m_record_count == MAX_RECORDS) {
        unlock();
        return false;
    }

    auto &record = m_records[m_record_count];
    record.m_callback = callback;
    record.m_argument = argument;
    record.m_scope = scope;
    record.m_kind = kind;
    record.m_active = true;
    ++m_record_count;
    unlock();
    return true;
}

auto Registry::register_destructor(Destructor destructor, void *argument, Scope scope) noexcept
    -> bool {
    if (destructor == nullptr) {
        return false;
    }
    Callback callback;
    callback.m_destructor = destructor;
    return register_callback(callback, argument, scope, CallbackKind::Destructor);
}

auto Registry::register_exit_function(ExitFunction function) noexcept -> bool {
    if (function == nullptr) {
        return false;
    }
    Callback callback;
    callback.m_exit_function = function;
    return register_callback(callback, nullptr, {}, CallbackKind::ExitFunction);
}

auto Registry::finalize(Scope scope) noexcept -> void {
    for (;;) {
        Callback callback;
        void *argument;
        CallbackKind kind;
        auto found = false;

        lock();
        for (auto index = m_record_count; index != 0; --index) {
            auto &candidate = m_records[index - 1];
            if (!matches(candidate, scope)) {
                continue;
            }

            callback = candidate.m_callback;
            argument = candidate.m_argument;
            kind = candidate.m_kind;
            candidate.m_active = false;
            found = true;
            break;
        }
        unlock();

        if (!found) {
            return;
        }
        if (kind == CallbackKind::Destructor) {
            callback.m_destructor(argument);
        } else {
            callback.m_exit_function();
        }
    }
}
} // namespace ccrt
