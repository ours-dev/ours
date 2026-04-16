/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_SYNC_TYPES_HPP
#define OURS_SYNC_TYPES_HPP 1

namespace ours::sync {
    class Mutex;

    class Event;
    class Semaphore;

    class WaitQueue;
    class OwnedWaitQueue;

    enum class Interruptible {
        Yes,
        No,
    };
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_TYPES_HPP