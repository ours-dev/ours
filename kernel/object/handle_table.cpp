/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/port_dispatcher.hpp>

#include <utility>

#include <heap/new.hpp>

namespace ours::object {
HandleTable::HandleTable()
    : m_koid(KoIdGenerator::generate()), m_random(0), m_lock(), m_handles(), m_count(0) {}

HandleTable::~HandleTable() {
    auto _ = clean();
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
            auto _ = m_handles.erase(m_handles.iterator_to(raw));
            raw.set_table_koid(0);
            m_count -= 1;
            raw.dispatcher()->cancel(&raw);
            handle = ustl::Box<Handle>::from_raw(&raw);
        }
        closed += 1;
    }
    return closed;
}

/// Where a handle value is minted: the encoding is arithmetic on the number,
/// and this is the only place the kernel makes a new ours::Uandle out of one.
/// A value a program passes in becomes an ours::Uandle in from_register()
/// (kernel/lib/syscall/src/argument.hpp), or as a handle array is copied from
/// user memory.
auto HandleTable::value_of(Handle const &handle) const -> ours::Uandle {
    return static_cast<ours::Uandle>(
        ((handle.base_value() << HANDLE_RESERVED_BITS) | HANDLE_FIXED_BITS_MASK) ^ m_random);
}

auto HandleTable::add(ustl::Box<Handle> handle) -> ours::Uandle {
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

/// Takes |handle| out of this table and hands it to the caller: unlinked,
/// table koid 0, its waits ended.
///
/// Every wait made through the handle ends here, as Cancelled: once the handle
/// is gone from this table nothing here can satisfy them, and a thread
/// blocked in object_wait_one on a handle another thread closed -- or wrote
/// into a channel -- would otherwise sleep forever. Done under the table lock,
/// before anyone else can see the handle gone.
///
/// Earlier than Zircon, which does not cancel in RemoveHandleLocked()
/// (handle_table.cc): a closed handle's waits end as the handle is destroyed,
/// and a transferred one's as the receiver's msg_get_handles() takes it into
/// its table (channel.cc). The effect is the same -- no wait survives the
/// handle leaving the process it was made in -- and doing it once, here,
/// covers both.
auto HandleTable::unlink_locked(Handle *const handle) -> ustl::Box<Handle> {
    auto _ = m_handles.erase(m_handles.iterator_to(*handle));
    handle->set_table_koid(0);
    m_count -= 1;
    handle->dispatcher()->cancel(handle);
    return ustl::Box<Handle>::from_raw(handle);
}

auto HandleTable::remove(ours::Uandle const value) -> ustl::Box<Handle> {
    Guard guard{m_lock};
    auto *const handle = lookup_locked(value);
    if (handle == nullptr) {
        return ustl::Box<Handle>();
    }
    return unlink_locked(handle);
}

/// Takes the handles |values| names out of this table for a message, in one
/// hold of the lock, into |out| (count entries; one per value).
///
/// Every handle found is removed, whatever else happens: a handle given to a
/// write is consumed, and one that fails a check below is not put back but
/// left in |out| for the caller to close with the rest. A value that names
/// nothing -- including a value named a second time, whose handle the first
/// removal already took -- leaves its slot empty. The first failure is the
/// answer, in the order the values were given:
///
///   BadHandle      the value names no handle here;
///   AccessDenied   the handle lacks Transfer;
///   NotSupported   the handle is |channel| itself -- a channel end queued on
///                  its own peer would keep both ends alive through their own
///                  queues, and could never be closed from outside.
///
/// Removed first and checked after, as Zircon's
/// get_handle_for_message_locked() does (user_handles.cc), so a check that
/// fails cannot leave the handle half-way.
auto HandleTable::remove_for_transfer(ours::Uandle const *const values, u32 const count,
                                      Dispatcher const *const channel, ustl::Box<Handle> *const out)
    -> Status {
    auto status = Status::Ok;
    auto const latch = [&status](Status const failure) {
        if (status == Status::Ok) {
            status = failure;
        }
    };

    Guard guard{m_lock};
    for (u32 i = 0; i < count; ++i) {
        auto *const handle = lookup_locked(values[i]);
        if (handle == nullptr) {
            latch(Status::BadHandle);
            continue;
        }
        out[i] = unlink_locked(handle);
        if (!out[i]->has_rights(Rights::Transfer)) {
            latch(Status::AccessDenied);
        } else if (out[i]->dispatcher().as_ptr() == channel) {
            latch(Status::NotSupported);
        }
    }
    return status;
}

/// Checks, in Zircon's order (handle_dup_replace(), lib/syscalls/
/// handle_ops.cc), and answers with the first that fails:
///
///   BadHandle     |value| names no handle in this table;
///   AccessDenied  the handle lacks Duplicate;
///   InvArgs       |rights| asks for a right the handle does not have;
///   NoResource    the calling process is at its handle quota, or the arena
///                 is full.
///
/// Rights::SameRights means "the handle's own rights", and only as the whole
/// word: SameRights | Read is not "the same, plus Read" but a request for
/// bit 31, which no handle has, and fails the subset test like any other
/// right the handle lacks. Asking for exactly the handle's rights, or for
/// none at all, is allowed.
///
/// Under this table's lock from the lookup to the clone, as Zircon holds
/// its table lock across the same span: the clone takes its reference to
/// the object from the source handle, which must not be closed in between
/// (Handle::clone()). The clone takes the arena's lock inside this one --
/// table, then arena -- and the arena's lock takes nothing further.
auto HandleTable::duplicate(ours::Uandle const value, Rights rights, ustl::Box<Handle> *const out)
    -> Status {
    Guard guard{m_lock};
    auto *const source = lookup_locked(value);
    if (source == nullptr) {
        return Status::BadHandle;
    }
    if (!source->has_rights(Rights::Duplicate)) {
        return Status::AccessDenied;
    }
    if (rights == Rights::SameRights) {
        rights = source->rights();
    } else if (!source->has_rights(rights)) {
        return Status::InvArgs;
    }
    *out = Handle::clone(source, rights);
    if (!*out) {
        return Status::NoResource;
    }
    return Status::Ok;
}

auto HandleTable::attach_observer(ours::Uandle const value, SignalObserver *const observer,
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

auto HandleTable::attach_port_observer(ours::Uandle const value, ours::Uandle const port_value,
                                       u64 const key, Signals const signals,
                                       WaitAsyncOptions const options) -> Status {
    Guard guard{m_lock};

    auto const *const port_handle = lookup_locked(port_value);
    if (port_handle == nullptr) {
        return Status::BadHandle;
    }
    auto port = port_handle->dispatcher().downcast<PortDispatcher>();
    if (!port) {
        return Status::BadType;
    }
    if (!port_handle->has_rights(Rights::Write)) {
        return Status::AccessDenied;
    }

    auto *const handle = lookup_locked(value);
    if (handle == nullptr) {
        return Status::BadHandle;
    }
    if (!handle->has_rights(Rights::Wait)) {
        return Status::AccessDenied;
    }

    // After both handles, so that a bad handle outranks a bad option, and
    // before anything is attached.
    if (options != WaitAsyncOptions::None) {
        return Status::InvArgs;
    }

    // Allocated under the table lock, as Zircon's MakePortObserver() is:
    // the handle has to stay in the table until the observer is on the
    // object, or a close in between would never cancel it.
    auto &object = handle->dispatcher();
    auto *const observer =
        new (mm::GAF_KERNEL) PortObserver(ustl::move(port), object.as_ptr_mut(), key, signals);
    if (observer == nullptr) {
        return Status::NoMemory;
    }

    // Level-triggered: if the object already asserts one of |signals| the
    // packet is queued now, under the object's lock, and the observer is
    // never linked.
    return object->attach_observer(observer, handle, signals);
}

auto HandleTable::lookup_locked(ours::Uandle const value) const -> Handle * {
    // Decoding is value_of() run backwards, arithmetic on the number.
    auto const number = std::to_underlying(value);

    // 1. The fixed bits.
    if ((number & HANDLE_FIXED_BITS_MASK) != HANDLE_FIXED_BITS_MASK) {
        return nullptr;
    }

    // 2. Undo this table's random, then drop the fixed bits.
    auto const base_value = (number ^ m_random) >> HANDLE_RESERVED_BITS;

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
