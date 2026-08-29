/// Copyright (c) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef OURS_TASK_CONCEPT_ARCH_THREAD_HPP
#define OURS_TASK_CONCEPT_ARCH_THREAD_HPP 1

#include <ours/task/types.hpp>

#include <utility>

namespace ours::task::concepts {
    template <typename T>
    concept ArchThread = requires {
        { T::switch_thread(std::declval<T *>(), std::declval<T *>()) } -> std::same_as<void>;
    };
} // namespace ours::task::concepts

#endif // #ifndef OURS_TASK_CONCEPT_ARCH_THREAD_HPP