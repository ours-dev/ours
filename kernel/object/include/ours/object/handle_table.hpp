/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_TABLE_HPP
#define OURS_OBJECT_HANDLE_TABLE_HPP 1

#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/types.hpp>

#include <ustl/box.hpp>
#include <ustl/rc.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
/// The handles one process holds, and the only way from a handle value in a
/// register to the object it names.
///
/// The Handles themselves live in the system-wide arena (handle.hpp); a table
/// only links the ones that are its own and answers for them. A value
/// resolves here after five checks (docs/architecture/object/handle-lookup.md),
/// each of which stops a different thing:
///
///   1. the low two bits are both set -- rules out zero and small integers
///      made up by hand;
///   2. XOR with this table's random value -- a program cannot compute
///      another's values or read the arena layout off its own (the random is
///      zero until there is a CPRNG to draw it from);
///   3. the index lies in the backed part of the arena;
///   4. the slot holds a live handle with exactly this base value -- a value
///      kept after close() fails here once the slot is reused;
///   5. that handle is in *this* table -- process A's values do not resolve
///      in process B.
///
/// Every failure is BadHandle; which check failed is not the caller's
/// business. A handle that resolves but is the wrong kind of object is
/// BadType, and one that lacks a right is AccessDenied -- in that order, as
/// Zircon's GetDispatcherWithRights().
///
/// Locking: one Mutex per table. No Handle is ever destroyed under it --
/// destroying the last handle to an object runs its on_zero_handles(), which
/// may take that object's lock -- so remove() hands the handle back and the
/// caller lets it go after the lock is released.
class HandleTable: public ustl::RefCounted<HandleTable, ustl::sync::ThreadSafePolicy> {
    using Self = HandleTable;

  public:
    HandleTable();
    ~HandleTable();

    [[nodiscard]]
    auto koid() const -> KoId {
        return m_koid;
    }

    /// The value a program will name |handle| by once it is in this table.
    ///
    /// A pure function of the handle's base value and this table's random,
    /// which is what lets a system call work out an out-parameter's value,
    /// copy it to the caller, and only then install the handle: if the copy
    /// fails, nothing was ever visible and nothing has to be taken back.
    [[nodiscard]]
    auto value_of(Handle const &handle) const -> HandleValue;

    /// Installs |handle| and returns its value.
    auto add(ustl::Box<Handle> handle) -> HandleValue;

    /// Takes the handle named |value| out of this table; empty if |value|
    /// names nothing here. The caller owns the handle now, and destroying it
    /// -- which closes it -- happens outside this table's lock.
    [[nodiscard]]
    auto remove(HandleValue value) -> ustl::Box<Handle>;

    /// The object |value| names, as a T, provided the handle grants every
    /// right in |required|.
    template <typename T>
    [[nodiscard]]
    auto get_dispatcher(HandleValue const value, Rights const required, ustl::Arc<T> *const out)
        -> Status {
        Guard guard{m_lock};
        auto const *const handle = lookup_locked(value);
        if (handle == nullptr) {
            return Status::BadHandle;
        }
        auto typed = handle->dispatcher().template downcast<T>();
        if (!typed) {
            return Status::BadType;
        }
        if (!handle->has_rights(required)) {
            return Status::AccessDenied;
        }
        *out = ustl::move(typed);
        return Status::Ok;
    }

    /// Starts |observer| watching the object |value| names for |signals|,
    /// provided the handle grants Wait, and reports the object in |object| so
    /// the caller can stop watching later.
    ///
    /// Under this table's lock, and that is the point of it being a verb here
    /// rather than a lookup followed by an attach: the handle cannot be closed
    /// between being found and being watched through. A close that comes after
    /// cancels the observer instead (remove() calls Dispatcher::cancel()).
    /// Lock order: this table, then the object -- as everywhere else.
    auto attach_observer(HandleValue value, SignalObserver *observer, Signals signals,
                         ustl::Arc<Dispatcher> *object) -> Status;

  private:
    /// The five checks. Null when any of them fails.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto lookup_locked(HandleValue value) const -> Handle *;

    USTL_NO_MOVEABLE_AND_COPYABLE(HandleTable);

    /// What every handle in this table carries as its table koid -- the fifth
    /// check, which keeps one process's values from resolving in another.
    KoId m_koid;

    /// Pre-shifted left by HANDLE_RESERVED_BITS, so that the XOR in
    /// value_of() and lookup_locked() leaves the two fixed low bits alone --
    /// Zircon's handle_table.cc shifts it the same way. Zero until there is a
    /// CPRNG to draw it from; the other four checks do not depend on it.
    u32 const m_random;

    mutable sync::Mutex m_lock;
    Handle::TableList m_handles TA_GUARDED(m_lock);
    u32 m_count TA_GUARDED(m_lock);
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_HANDLE_TABLE_HPP