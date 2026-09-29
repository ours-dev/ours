/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_THREAD_HPP
#define OU_THREAD_HPP 1

#include <ou/str.hpp>

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
/// Makes a thread of |process| (which must grant ManageThread) that is not
/// running yet, and puts a handle to it in |out|. Closing that handle before
/// the thread is started leaves nothing behind. Zircon zx_thread_create().
auto thread_create(ours::HandleValue process, Str name, ours::HandleValue *out) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_THREAD_HPP
