/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_TIMED_LOOP_HPP
#define KTL_TIMED_LOOP_HPP 1

#include <ustl/function/invoke.hpp>
#include <ustl/util/move.hpp>

namespace ktl {
    template <typename TimeTraits, usize NumIterPerTimeRead>
    class TimedLoop {
        using Self = TimedLoop;
      public:
        using Clock = typename TimeTraits::Clock;
        using TimePoint = typename Clock::TimePoint;

        template <typename F, typename... Args>
        auto with(F &&f, Args &&...args) -> void {
            while (!is_exceeded()) {
                ustl::function::invoke(f, ustl::forward<Args>(args)...);
            }
        }

        auto is_exceeded() -> bool {}
      private:
        TimePoint m_deadline;
    };

} // namespace ktl

#endif // #ifndef KTL_TIMED_LOOP_HPP