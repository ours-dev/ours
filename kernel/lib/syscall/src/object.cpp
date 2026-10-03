/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/process_dispatcher.hpp>
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
    // deadline other than 0 and INSTANT_INFINITE. Optional, as in Zircon.
    auto const ended =
        status == Status::Ok || status == Status::TimeOut || status == Status::Cancelled;
    if (ended && !observed.is_null()) {
        if (auto const copied = observed.copy_to_user(current); copied != Status::Ok) {
            return copied;
        }
    }
    return status;
}

namespace {
    /// Writes |record|, the one record a single-record topic has, and the
    /// counts that go with it.
    ///
    /// |actual| and |avail| are written whether or not the record fits: avail
    /// is how a caller with too small a buffer learns what to ask for. The
    /// record goes first, so a buffer that cannot be written answers InvArgs
    /// with nothing else touched.
    ///
    /// Zircon: single_record_result() and actual_avail_result()
    /// (kernel/lib/syscalls/object_info.rs).
    template <typename Record>
    auto single_record_result(user_copy::UserOutPtr<u8> const buffer, usize const buffer_size,
                              user_copy::UserOutPtr<usize> const actual,
                              user_copy::UserOutPtr<usize> const avail, Record const &record)
        -> Status {
        usize written = 0;
        if (buffer_size >= sizeof(Record)) {
            // The buffer arrives as bytes, since its type depends on the
            // topic; this topic's type is known, so the pointer is retyped.
            // Zircon does the same (UserOutPtr::reinterpret).
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
} // namespace

/// The topic decides everything, the object type and right included, so it is
/// looked at first: an unknown topic is NotSupported before the handle is,
/// as in Zircon, whose default case answers ZX_ERR_NOT_SUPPORTED without a
/// lookup.
///
/// Zircon: sys_object_get_info() (kernel/lib/syscalls/object_info.rs), whose
/// ZX_INFO_PROCESS case is ProcessDispatcher::GetInfo() under Inspect.
auto object_get_info(ours::Uandle const handle, InfoTopic const topic,
                     user_copy::UserOutPtr<u8> const buffer, usize const buffer_size,
                     user_copy::UserOutPtr<usize> const actual,
                     user_copy::UserOutPtr<usize> const avail) -> Status {
    switch (topic) {
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
        default:
            return Status::NotSupported;
    }
}
} // namespace ours::syscall
