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
} // namespace ours

#endif // #ifndef OURS_STATUS_HPP
