/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_ERROR_HPP
#define USTL_COLLECTIONS_ERROR_HPP 1

namespace ustl::collections {
enum class Error {
    OutOfMemory,
    CapacityOverflow,
    InvArgs,
    AlreadyExists,
};
} // namespace ustl::collections

#endif // USTL_COLLECTIONS_ERROR_HPP
