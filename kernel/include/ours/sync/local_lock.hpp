/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_LOCAL_LOCK_HPP
#define OURS_SYNC_LOCAL_LOCK_HPP 1

#include <ours/sync/config.hpp>

namespace ours::sync {
class TA_CAPABILITY("LocalLock") LocalLock {
    using Self = LocalLock;

  public:
    TA_ACQUIRE() auto acquire() -> void;
    TA_RELEASE() auto release() -> void;

  private:
};
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_LOCAL_LOCK_HPP