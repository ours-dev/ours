/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/handle_table.hpp>

namespace ours::object {
HandleTable::HandleTable()
    : m_koid(KoIdGenerator::generate()), m_random(0), m_lock(), m_handles(), m_count(0) {}

HandleTable::~HandleTable() {
    static_cast<void>(clean());
}

/// Closes every handle in the table, one at a time: each is taken out under
/// the lock and destroyed after it, because closing the last handle to an
/// object runs its on_zero_handles(), which must not happen under a table
/// lock (see the class comment).
///
/// Zircon's HandleTable::Clean() swaps the whole list out under the lock and
/// then destroys it outside, in one pass each. One at a time here because a
/// Handle is linked by an intrusive hook into exactly one list, and there is
/// no second list type to swap into; the cost is a lock round trip per handle,
/// on a path taken once per process.
auto HandleTable::clean() -> u32 {
    u32 closed = 0;
    for (;;) {
        ustl::Box<Handle> handle;
        {
            Guard guard{m_lock};
            auto front = m_handles.front();
            if (!front) {
                break;
            }
            auto &raw = *front;
            static_cast<void>(m_handles.erase(m_handles.iterator_to(raw)));
            raw.set_table_koid(0);
            m_count -= 1;
            raw.dispatcher()->cancel(&raw);
            handle = ustl::Box<Handle>::from_raw(&raw);
        }
        closed += 1;
    }
    return closed;
}

auto HandleTable::value_of(Handle const &handle) const -> HandleValue {
    return ((handle.base_value() << HANDLE_RESERVED_BITS) | HANDLE_FIXED_BITS_MASK) ^ m_random;
}

auto HandleTable::add(ustl::Box<Handle> handle) -> HandleValue {
    DEBUG_ASSERT(handle);
    auto *const raw = handle.into_raw();
    DEBUG_ASSERT(raw->table_koid() == 0, "a handle is in at most one table");
    auto const value = value_of(*raw);

    Guard guard{m_lock};
    raw->set_table_koid(m_koid);
    m_handles.push_back(*raw);
    m_count += 1;
    return value;
}

auto HandleTable::remove(HandleValue const value) -> ustl::Box<Handle> {
    Guard guard{m_lock};
    auto *const handle = lookup_locked(value);
    if (handle == nullptr) {
        return ustl::Box<Handle>();
    }
    static_cast<void>(m_handles.erase(m_handles.iterator_to(*handle)));
    handle->set_table_koid(0);
    m_count -= 1;

    // Every wait made through this handle ends here, as Cancelled: once the
    // handle is gone nothing can satisfy them, and a thread blocked in
    // object_wait_one on a handle another thread closed would otherwise sleep
    // forever. Done under the table lock, before anyone else can see the
    // handle gone -- Zircon's HandleTable::RemoveHandleLocked() cancels at the
    // same point.
    handle->dispatcher()->cancel(handle);

    return ustl::Box<Handle>::from_raw(handle);
}

auto HandleTable::attach_observer(HandleValue const value, SignalObserver *const observer,
                                  Signals const signals, ustl::Arc<Dispatcher> *const object)
    -> Status {
    Guard guard{m_lock};
    auto *const handle = lookup_locked(value);
    if (handle == nullptr) {
        return Status::BadHandle;
    }
    if (!handle->has_rights(Rights::Wait)) {
        return Status::AccessDenied;
    }
    *object = handle->dispatcher();
    return (*object)->attach_observer(observer, handle, signals);
}

auto HandleTable::lookup_locked(HandleValue const value) const -> Handle * {
    // 1. The fixed bits.
    if ((value & HANDLE_FIXED_BITS_MASK) != HANDLE_FIXED_BITS_MASK) {
        return nullptr;
    }

    // 2. Undo this table's random, then drop the fixed bits.
    auto const base_value = (value ^ m_random) >> HANDLE_RESERVED_BITS;

    // 3 and 4. In the backed arena, and a live handle with this base value.
    auto *const handle = Handle::from_base_value(base_value);
    if (handle == nullptr) {
        return nullptr;
    }

    // 5. And it is one of ours.
    if (handle->table_koid() != m_koid) {
        return nullptr;
    }
    return handle;
}

} // namespace ours::object
