/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_HPP
#define OURS_TASK_HPP 1

// The thread and task calls, one domain in one file: making, starting and
// ending threads, stopping one for as long as a token lives, and ending a
// process from outside. One header because one kernel file holds them all
// (kernel/lib/syscall/src/task.cpp), which is what a category is named for;
// a program that touches threads or tasks includes this and nothing else of
// the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
/// thread_create's options: None only, anything else InvArgs.
enum class ThreadCreateOptions : u32 {
    None = 0,
};

// The thread and task calls, as a program calls them: generated from
// //system/public/ours/syscalls/task.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/task.inc>
} // namespace ours

#endif // OURS_TASK_HPP
