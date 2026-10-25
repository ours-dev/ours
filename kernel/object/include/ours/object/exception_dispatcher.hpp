/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_EXCEPTION_DISPATCHER_HPP
#define OURS_OBJECT_EXCEPTION_DISPATCHER_HPP 1

#include <ours/exception.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>
#include <ours/sync/event.hpp>

#include <ustl/rc.hpp>

namespace ours::object {
/// Rights a freshly created exception handle carries. No Duplicate, so there
/// is only ever one handle to an exception and its close is the handler's
/// answer; no Wait, because nothing is signalled on it.
constexpr auto RIGHTS_DEFAULT_EXCEPTION =
    Rights::Transfer | Rights::GetProperty | Rights::SetProperty | Rights::Inspect;

using ExceptionDispatcherBase = SoloDispatcher<ExceptionDispatcher, RIGHTS_DEFAULT_EXCEPTION>;

/// One exception in flight: a thread stopped in the kernel, waiting for a
/// handler's answer.
///
/// The handler holds the one handle to it. Closing that handle is the
/// answer, and the ExceptionState set on it before the close says what the
/// answer is. The faulting thread creates the object, sends it
/// (Exceptionate::send()), and waits in wait_for_handle_close() while the
/// handle is out; it holds a reference throughout, so the object outlives
/// both the wait and a handler that closes late.
///
/// Left out until something uses them: the exception report and register
/// context, and second chance.
class ExceptionDispatcher final: public ExceptionDispatcherBase {
    using Base = ExceptionDispatcherBase;
    using Self = ExceptionDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Exception);

    /// What the handler's close means for the faulting thread.
    enum class Response {
        /// ExceptionState::Handled: the thread resumes.
        Handled,
        /// ExceptionState::TryNext, the default: on to the next handler.
        TryNext,
        /// The thread was killed while it waited. Whatever the handler set
        /// no longer matters: a kill always wins.
        Killed,
    };

    /// An exception of |type| raised by |thread|. Null when out of memory.
    static auto create(ustl::Arc<ThreadDispatcher> thread, ExceptionType type) -> ustl::Arc<Self>;

    /// The thread that raised it. Held for as long as the exception is.
    [[nodiscard]]
    auto thread() const -> ustl::Arc<ThreadDispatcher> const & {
        return m_thread;
    }

    [[nodiscard]]
    auto exception_type() const -> ExceptionType {
        return m_type;
    }

    /// Records the handler's decision, to take effect when it closes the
    /// handle: object_set_property(Property::ExceptionState).
    auto set_state(ExceptionState state) -> void;

    /// The rights the thread and process handles the next handler can ask
    /// for get (make_thread_handle(), make_process_handle()); None means it
    /// may not have that one. Set by the exceptionate that sends this,
    /// before the handle leaves the kernel, from the rights of the task
    /// handle its channel was bound with -- so a handler never gets more than
    /// whoever bound the channel had. One handler holds the exception at a
    /// time, so the rights never change while one is looking.
    auto set_task_rights(Rights thread_rights, Rights process_rights) -> void;

    /// A new handle to the faulting thread, with the rights set above, charged
    /// to the calling process: AccessDenied when its handler may not have one,
    /// NoResource at the caller's quota or a full arena. exception_get_thread.
    auto make_thread_handle(ustl::Box<Handle> *out) -> Status;

    /// The same for the thread's process: exception_get_process. A
    /// thread-level channel gives none (AccessDenied): it grants no way up the
    /// task chain.
    auto make_process_handle(ustl::Box<Handle> *out) -> Status;

    /// The handle is gone, wherever it was -- closed by the handler, freed
    /// unread with its channel, or closed with the handler's process: the
    /// handler is done.
    auto on_zero_handles() -> void override;

    /// Blocks the faulting thread until the handler closes the handle, or
    /// until the thread is killed. Called exactly once after each send that
    /// succeeded, by the thread that sent; it resets the state for the next
    /// handler.
    ///
    /// Interruptible, with Suspend masked: a kill ends the wait, a suspend
    /// does not -- the thread stays here, in its exception, until both are
    /// over.
    [[nodiscard]]
    auto wait_for_handle_close() -> Response;

    /// For a send that minted the handle and then failed: the handle died
    /// without reaching a handler, and its on_zero_handles() signalled an
    /// answer nobody gave. Takes that back, so that the next send's wait
    /// waits.
    auto discard_handle_close() -> void;

  private:
    ExceptionDispatcher(ustl::Arc<ThreadDispatcher> thread, ExceptionType type);
    ~ExceptionDispatcher() override;

    ustl::Arc<ThreadDispatcher> const m_thread;
    ExceptionType const m_type;

    /// What the handler decided.
    ExceptionState m_state TA_GUARDED(get_lock());

    /// See set_task_rights().
    Rights m_thread_rights TA_GUARDED(get_lock());
    Rights m_process_rights TA_GUARDED(get_lock());

    /// Signalled by on_zero_handles(). Manual-reset, and unsignalled again by
    /// the one thread that waits on it once it has the answer, and by
    /// discard_handle_close(). With one handle out at a time that is the same
    /// as an AutoUnsignal event, an option Event did not have when this was
    /// written.
    sync::Event m_response;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_EXCEPTION_DISPATCHER_HPP
