/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VERSION_HPP
#define OURS_VERSION_HPP 1

namespace ours {
/// Pointer to the NUL-terminated kernel version. The bytes live in the vDSO.
using KernelVersion = char const *;
} // namespace ours

#endif // #ifndef OURS_VERSION_HPP
