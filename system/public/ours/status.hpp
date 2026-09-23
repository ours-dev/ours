/// Copyright (c) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef OURS_STATUS_HPP
#define OURS_STATUS_HPP 1

#include <ours/types.hpp>

namespace ours {
/// Every value is written out, because these numbers cross the system-call
/// boundary in %rax and a program compiled against one build of this header
/// has to mean the same thing by them as a kernel built against the next.
/// They were implicit, so inserting a status anywhere but the end would have
/// silently renumbered everything after it. Aliases (Unsupported,
/// OutOfMem...) carry no number of their own.
enum class Status : u32 {
    /// Indicates a successful operation.
    Ok = 0,

    /// The operation is delayed and will be performed later.
    /// For example, the operation is enqueued to a DPC queue.
    Delayed = 1,

    /// The host, device, or service is not reachable.
    NotReachable = 2,

    /// The operation is not supported, implemented or enabled.
    NotSupported = 3,
    Unsupported = NotSupported,
    Unimplemented = NotSupported,

    /// The object is not found.
    NotFound = 4,

    /// The memory is not enough.
    NoMemory = 5,
    OutOfMem = NoMemory,

    /// The filesystem or device space is exhausted.
    NoSpace = 6,

    /// The resource, like peripherals or network interfaces, is not available.
    NoResource = 7,

    /// The object, like a file or directory, already exists.
    AlreadyExists = 8,

    /// The operation failed because the named entity is already owned or controlled
    /// by another entity. The operation could succeed later if the current owner
    /// releases the entity.
    AlreadyBound = 9,

    InvArgs = 10,

    /// An argument is outside the valid range for this operation.
    OutOfRange = 11,

    /// The time limit for the operation elapsed before the operation completed.
    TimeOut = 12,

    /// The operation should wait for a condition to be met.
    ShouldWait = 13,

    /// The operation should retry.
    ShouldRetry = 14,

    /// The operation was cancelled.
    Cancelled = 15,

    /// The operation failed because the current state of the object does not allow
    /// it, or a precondition of the operation is not satisfied.
    BadState = 16,
    BadHandle = 17,
    BadType = 18,
    BadSyscall = 19,

    /// The operation is not allowed due to insufficient permissions.
    AccessDenied = 20,

    /// The path is not a directory.
    BadPath = 21,
    NotDir = 22,
    NotFile = 23,
    NotEmpty = 24,

    NotConnected = 25,
    ConnectionRefused = 26,
    ConnectionAborted = 27,
    ConnectionReset = 28,

    /// The system encountered an otherwise unspecified error while performing the
    /// operation.
    InternalError = 29,

    InternalIrqKilled = 30,
    InternalIrqRetry = 31,

    /// The maximum number of status values.
    /// A peer object -- the other end of a channel -- has gone: every handle
    /// to it was closed. Distinct from BadHandle (this caller's handle is
    /// fine) and from BadState (nothing this caller can do will change it).
    /// Zircon ZX_ERR_PEER_CLOSED.
    PeerClosed = 32,

    /// The caller's buffer cannot hold what there is to return, and the
    /// operation left it where it was so the caller can retry with a larger
    /// one -- channel_read reports the sizes it needed. Not NoSpace, which is
    /// about a filesystem or device filling up. Zircon ZX_ERR_BUFFER_TOO_SMALL.
    BufferTooSmall = 33,

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
        case Status::InvArgs:
            return "InvArgs";
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
        case Status::PeerClosed:
            return "PeerClosed";
        case Status::BufferTooSmall:
            return "BufferTooSmall";
        case Status::MaxCount:
            return "MaxCount";
    }
    return "Unknown";
}
} // namespace ours

#endif // #ifndef OURS_STATUS_HPP
