/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <span>

#include <ktl/vec.hpp>
#include <ours/object/exception_dispatcher.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/object/wait.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
auto object_wait_one(ours::Uandle const handle, Signals const signals, Instant const deadline,
                     user_copy::UserOutPtr<Signals> const observed) -> Status {
    Signals current = Signals::None;
    auto const status = object::wait_one(handle, signals, deadline, &current);

    // What was asserted is reported for every way a wait can end -- matched,
    // timed out, or cancelled by the handle leaving the table -- since each is
    // a real answer about the object. Not for a wait that never began: a bad
    // handle or a missing right, where there is no object to report on, or a
    // deadline other than 0 and INSTANT_INFINITE. |observed| is optional.
    auto const ended =
        status == Status::Ok || status == Status::TimeOut || status == Status::Cancelled;
    if (ended && !observed.is_null()) {
        if (auto const copied = observed.copy_to_user(current); copied != Status::Ok) {
            return copied;
        }
    }
    return status;
}

/// The work is HandleTable::attach_port_observer(), which has to hold the
/// table's lock across both lookups and the attach.
auto object_wait_async(ours::Uandle const handle, ours::Uandle const port, u64 const key,
                       Signals const signals, WaitAsyncOptions const options) -> Status {
    return object::ProcessDispatcher::current()->handle_table().attach_port_observer(
        handle, port, key, signals, options);
}

namespace {
    /// Writes |record|, the one record a single-record topic has, and the
    /// counts that go with it.
    ///
    /// |actual| and |avail| are written whether or not the record fits: avail
    /// is how a caller with too small a buffer learns what to ask for. The
    /// record goes first, so a buffer that cannot be written answers InvArgs
    /// with nothing else touched.
    template <typename Record>
    auto single_record_result(user_copy::UserOutPtr<u8> const buffer, usize const buffer_size,
                              user_copy::UserOutPtr<usize> const actual,
                              user_copy::UserOutPtr<usize> const avail, Record const &record)
        -> Status {
        usize written = 0;
        if (buffer_size >= sizeof(Record)) {
            // The buffer arrives as bytes, since its type depends on the
            // topic; this topic's type is known, so the pointer is retyped.
            auto const typed = user_copy::UserOutPtr<Record>(buffer.address());
            if (auto const status = typed.copy_to_user(record); status != Status::Ok) {
                return status;
            }
            written = 1;
        }
        if (!actual.is_null()) {
            if (auto const status = actual.copy_to_user(written); status != Status::Ok) {
                return status;
            }
        }
        if (!avail.is_null()) {
            if (auto const status = avail.copy_to_user(usize{1}); status != Status::Ok) {
                return status;
            }
        }
        return written == 0 ? Status::BufferTooSmall : Status::Ok;
    }

    /// Writes |records|, as many of a multi-record topic's records as fit the
    /// caller's buffer, then the counts: |actual| the records written,
    /// |avail| all there are. A short buffer is not an error, as it is for a
    /// single record: |avail| tells the caller how much to ask for, and the
    /// records it got are still good. The records go first, so a buffer that
    /// cannot be written answers its fault with nothing else touched.
    template <typename Record>
    auto multi_record_result(user_copy::UserOutPtr<u8> const buffer,
                             user_copy::UserOutPtr<usize> const actual,
                             user_copy::UserOutPtr<usize> const avail,
                             std::span<Record const> const records, usize const available)
        -> Status {
        if (!records.empty()) {
            auto const typed = user_copy::UserOutPtr<Record>(buffer.address());
            if (auto const status = typed.copy_array_to_user(records.data(), records.size());
                status != Status::Ok) {
                return status;
            }
        }
        if (!actual.is_null()) {
            if (auto const status = actual.copy_to_user(records.size()); status != Status::Ok) {
                return status;
            }
        }
        if (!avail.is_null()) {
            if (auto const status = avail.copy_to_user(available); status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }
} // namespace

/// The topic decides everything, the object type and right included, so it is
/// looked at first: an unknown topic is NotSupported before the handle is
/// looked up.
auto object_get_info(ours::Uandle const handle, InfoTopic const topic,
                     user_copy::UserOutPtr<u8> const buffer, usize const buffer_size,
                     user_copy::UserOutPtr<usize> const actual,
                     user_copy::UserOutPtr<usize> const avail) -> Status {
    switch (topic) {
        case InfoTopic::HandleBasic: {
            ustl::Arc<object::Dispatcher> dispatcher;
            auto rights = Rights::None;
            if (auto const status =
                    object::ProcessDispatcher::current()->handle_table().get_untyped(
                        handle, &dispatcher, &rights);
                status != Status::Ok) {
                return status;
            }
            // Outside the table's lock: the koid, the type and the related
            // koid are all fixed when the object is made.
            auto const record = InfoHandleBasic{
                .koid = dispatcher->koid(),
                .rights = rights,
                .type = dispatcher->type(),
                .related_koid = dispatcher->related_koid(),
                .padding1 = {},
            };
            return single_record_result(buffer, buffer_size, actual, avail, record);
        }
        case InfoTopic::Process: {
            ustl::Arc<object::ProcessDispatcher> process;
            if (auto const status =
                    object::ProcessDispatcher::current()->handle_table().get_dispatcher(
                        handle, Rights::Inspect, &process);
                status != Status::Ok) {
                return status;
            }
            return single_record_result(buffer, buffer_size, actual, avail, process->get_info());
        }
        case InfoTopic::JobProcesses: {
            ustl::Arc<object::JobDispatcher> job;
            if (auto const status =
                    object::ProcessDispatcher::current()->handle_table().get_dispatcher(
                        handle, Rights::Enumerate, &job);
                status != Status::Ok) {
                return status;
            }
            ktl::Vec<KoId> koids;
            usize available = 0;
            if (auto const status =
                    job->get_process_koids(buffer_size / sizeof(KoId), &koids, &available);
                status != Status::Ok) {
                return status;
            }
            return multi_record_result(buffer, actual, avail,
                                       std::span<KoId const>{koids.data(), koids.size()},
                                       available);
        }
        case InfoTopic::ProcessMaps: {
            ustl::Arc<object::ProcessDispatcher> process;
            if (auto const status =
                    object::ProcessDispatcher::current()->handle_table().get_dispatcher(
                        handle, Rights::Inspect, &process);
                status != Status::Ok) {
                return status;
            }
            ktl::Vec<InfoMaps> records;
            usize available = 0;
            if (auto const status =
                    process->get_maps(buffer_size / sizeof(InfoMaps), &records, &available);
                status != Status::Ok) {
                return status;
            }
            return multi_record_result(buffer, actual, avail,
                                       std::span<InfoMaps const>{records.data(), records.size()},
                                       available);
        }
        case InfoTopic::Resource: {
            ustl::Arc<object::ResourceDispatcher> resource;
            if (auto const status =
                    object::ProcessDispatcher::current()->handle_table().get_dispatcher(
                        handle, Rights::Inspect, &resource);
                status != Status::Ok) {
                return status;
            }
            return single_record_result(buffer, buffer_size, actual, avail, resource->get_info());
        }
        default:
            return Status::NotSupported;
    }
}

/// Zircon's order: the parent with Enumerate (AccessDenied), the rights asked
/// a subset of the parent's (AccessDenied), then the child by type -- a job's
/// process, a process's thread (BadType for any other parent, NotFound for
/// no such child). The handle is made from a reference of its own, never a
/// KernelHandle: dropping that on a failure would tell a child that still has
/// other handles it had lost its last one, and kill a process not yet started.
auto object_get_child(ours::Uandle const handle, KoId const koid, Rights const rights,
                      user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::Dispatcher> parent;
    auto parent_rights = Rights::None;
    if (auto const status = table.get_untyped(handle, &parent, &parent_rights);
        status != Status::Ok) {
        return status;
    }
    if (!(parent_rights & Rights::Enumerate)) {
        return Status::AccessDenied;
    }
    auto granted = parent_rights;
    if (rights != Rights::SameRights) {
        if (!!(rights & ~parent_rights)) {
            return Status::AccessDenied;
        }
        granted = rights;
    }

    ustl::Arc<object::Dispatcher> child;
    if (auto job = parent.downcast<object::JobDispatcher>(); job) {
        auto process = job->lookup_process(koid);
        if (!process) {
            return Status::NotFound;
        }
        child = ustl::Arc<object::Dispatcher>(ustl::move(process));
    } else if (auto process = parent.downcast<object::ProcessDispatcher>(); process) {
        auto thread = process->lookup_thread(koid);
        if (!thread) {
            return Status::NotFound;
        }
        child = ustl::Arc<object::Dispatcher>(ustl::move(thread));
    } else {
        return Status::BadType;
    }

    auto made = object::Handle::make(ustl::move(child), granted);
    if (!made) {
        return Status::NoResource;
    }
    // The value goes out before the handle goes in, as port_create does: a
    // fault here leaves no handle behind.
    if (auto const status = out.copy_to_user(table.value_of(*made)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(made));
    return Status::Ok;
}

/// The property decides the object type, so it is looked at first, as in
/// object_get_info(); an unknown property is InvArgs, and it comes before
/// the handle is looked up: the table has no lookup that does not name the
/// type.
auto object_set_property(ours::Uandle const handle, Property const property,
                         user_copy::UserInPtr<u8 const> const value, usize const size) -> Status {
    switch (property) {
        case Property::ExceptionState: {
            ustl::Arc<object::ExceptionDispatcher> exception;
            if (auto const status =
                    object::ProcessDispatcher::current()->handle_table().get_dispatcher(
                        handle, Rights::SetProperty, &exception);
                status != Status::Ok) {
                return status;
            }
            if (size < sizeof(ExceptionState)) {
                return Status::BufferTooSmall;
            }
            // As the plain number it arrives as: an ExceptionState read
            // straight out of user memory could hold any value at all.
            u32 raw = 0;
            if (auto const status =
                    user_copy::UserInPtr<u32 const>(value.address()).copy_from_user(&raw);
                status != Status::Ok) {
                return status;
            }
            auto const state = static_cast<ExceptionState>(raw);
            if (state != ExceptionState::TryNext && state != ExceptionState::Handled) {
                return Status::InvArgs;
            }
            exception->set_state(state);
            return Status::Ok;
        }
        default:
            return Status::InvArgs;
    }
}
} // namespace ours::syscall
