/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_PRIV_LOCK_NAME_STORAGE_HPP
#define OURS_SYNC_PRIV_LOCK_NAME_STORAGE_HPP 1

namespace ours::sync::priv {
    template <typename LockType, bool On>
    class LockNameStorage;

    template <typename LockType>
    class LockNameStorage<LockType, true> {};

    template <typename LockType>
    class LockNameStorage<LockType, false> {};
} // namespace ours::sync::priv

#endif // #ifndef OURS_SYNC_PRIV_LOCK_NAME_STORAGE_HPP