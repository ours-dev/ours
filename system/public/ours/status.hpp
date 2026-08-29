/// Copyright (c) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef OURS_STATUS_HPP
#define OURS_STATUS_HPP 1

#include <ours/types.hpp>

namespace ours {
enum class Status : u32 {
    /// Indicates a successful operation.
    Ok,

    /// The operation is delayed and will be performed later.
    /// For example, the operation is enqueued to a DPC queue.
    Delayed,

    /// The host, device, or service is not reachable.
    NotReachable,

    /// The operation is not supported, implemented or enabled.
    NotSupported,
    Unsupported = NotSupported,
    Unimplemented = NotSupported,

    /// The object is not found.
    NotFound,

    /// The memory is not enough.
    NoMemory,
    OutOfMem = NoMemory,

    /// The filesystem or device space is exhausted.
    NoSpace,

    /// The resource, like peripherals or network interfaces, is not available.
    NoResource,

    /// The object, like a file or directory, already exists.
    AlreadyExists,

    /// The operation failed because the named entity is already owned or controlled
    /// by another entity. The operation could succeed later if the current owner
    /// releases the entity.
    AlreadyBound,

    InvalidArgument,
    InvalidArguments = InvalidArgument,

    /// An argument is outside the valid range for this operation.
    OutOfRange,

    /// The time limit for the operation elapsed before the operation completed.
    TimeOut,

    /// The operation should wait for a condition to be met.
    ShouldWait,

    /// The operation should retry.
    ShouldRetry,

    /// The operation was cancelled.
    Cancelled,

    /// The operation failed because the current state of the object does not allow
    /// it, or a precondition of the operation is not satisfied.
    BadState,
    BadHandle,
    BadType,
    BadSyscall,

    /// The operation is not allowed due to insufficient permissions.
    AccessDenied,

    /// The path is not a directory.
    BadPath,
    NotDir,
    NotFile,
    NotEmpty,

    NotConnected,
    ConnectionRefused,
    ConnectionAborted,
    ConnectionReset,

    /// The system encountered an otherwise unspecified error while performing the
    /// operation.
    InternalError,

    InternalIrqKilled,
    InternalIrqRetry,

    /// The maximum number of status values.
    MaxCount,
};

inline auto to_string(Status status) -> char const * {
    switch (status) {
        case Status::Ok:
            return "Ok";
        case Status::Delayed:
            return "Delayed";
        case Status::NotReachable:
            return "NotReachable";
        case Status::NotSupported:
            return "NotSupported";
        case Status::NotFound:
            return "NotFound";
        case Status::NoMemory:
            return "NoMemory";
        case Status::NoSpace:
            return "NoSpace";
        case Status::NoResource:
            return "NoResource";
        case Status::AlreadyExists:
            return "AlreadyExists";
        case Status::AlreadyBound:
            return "AlreadyBound";
        case Status::InvalidArgument:
            return "InvalidArgument";
        case Status::OutOfRange:
            return "OutOfRange";
        case Status::TimeOut:
            return "TimeOut";
        case Status::ShouldWait:
            return "ShouldWait";
        case Status::ShouldRetry:
            return "ShouldRetry";
        case Status::Cancelled:
            return "Cancelled";
        case Status::BadState:
            return "BadState";
        case Status::BadHandle:
            return "BadHandle";
        case Status::BadType:
            return "BadType";
        case Status::BadSyscall:
            return "BadSyscall";
        case Status::AccessDenied:
            return "AccessDenied";
        case Status::BadPath:
            return "BadPath";
        case Status::NotDir:
            return "NotDir";
        case Status::NotFile:
            return "NotFile";
        case Status::NotEmpty:
            return "NotEmpty";
        case Status::NotConnected:
            return "NotConnected";
        case Status::ConnectionRefused:
            return "ConnectionRefused";
        case Status::ConnectionAborted:
            return "ConnectionAborted";
        case Status::ConnectionReset:
            return "ConnectionReset";
        case Status::InternalError:
            return "InternalError";
        case Status::InternalIrqKilled:
            return "InternalIrqKilled";
        case Status::InternalIrqRetry:
            return "InternalIrqRetry";
        case Status::MaxCount:
            return "MaxCount";
    }
    return "Unknown";
}
} // namespace ours

#endif // #ifndef OURS_STATUS_HPP
