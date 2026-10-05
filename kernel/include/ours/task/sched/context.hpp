/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_CONTEXT_HPP
#define OURS_TASK_SCHED_CONTEXT_HPP 1

#include <ours/task/sched/sched_entity.hpp>

namespace ours::task::sched {
/// Migration shim for `fair.*`, which is not mine to edit.
///
/// `Context` was renamed to `SchedEntity`: "context" named nothing -- a
/// context of what? -- while the object is the thread's scheduling entity:
/// everything the scheduler keeps about one thread, its profile, affinity,
/// accounting and each discipline's own state.
///
/// Everything outside `fair.hpp` and `fair.cpp` was renamed. Those two are the
/// scheduler author's, so this header and the alias below keep them compiling
/// untouched: they include `sched/context.hpp` and spell the type `Context`
/// thirty times between them.
///
/// **Delete this file, and `Thread::scheduling_context()`, when Fair is brought
/// back and renamed by hand.** Nothing else includes it.
using Context = SchedEntity;
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_CONTEXT_HPP
