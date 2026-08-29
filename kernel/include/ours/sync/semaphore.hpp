/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_SEMAPHORE_HPP
#define OURS_SYNC_SEMAPHORE_HPP 1

#include <ours/status.hpp>
#include <ours/task/wait_queue.hpp>

namespace ours::sync {
    class Semaphore {
        using Self = Semaphore;
      public:
        auto wait() -> Status;
        auto post() -> void;
      private:
        task::WaitQueue m_waiters;
    };
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SEMAPHORE_HPP