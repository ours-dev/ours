/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_TYPES_HPP
#define OURS_TASK_TYPES_HPP 1

#include <ours/types.hpp>

#include <ustl/function/fn.hpp>

namespace ours::task {
    using ThreadStartRoutine = ustl::function::Fn<auto () -> i32>;
    using ThreadTrampolineRoutine = auto () -> void;

    class Thread;
    class IdleThread;

    class AutoPreemptDisabler;
    class AutoEagerReschedDisabler;

} // namespace ours::task

#endif // #ifndef OURS_TASK_TYPES_HPP