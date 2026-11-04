/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KERNEL_IRQ_LINE_ACCESS_HPP
#define KERNEL_IRQ_LINE_ACCESS_HPP 1

#include <ours/task/irq_rcu.hpp>

#include <functional>
#include <type_traits>
#include <utility>

#include <kernel/kernel/irq/line.hpp>

namespace ours::irq {
/// Resolve and use a line in one reader scope. Both a private Domain and the
/// registry provide find_line(id, guard); neither caller repeats the lifetime
/// protocol. A callback's Status or Result carries lookup failures unchanged.
template <typename Source, typename Operation>
    requires std::is_invocable_v<Operation, Line &>
[[nodiscard]]
auto with_line(Source &source, VIrqNum const id, Operation &&operation)
    -> std::invoke_result_t<Operation, Line &> {
    using Return = std::invoke_result_t<Operation, Line &>;
    task::IrqRcuReadGuard const guard;
    auto const line = source.find_line(id, guard);
    if (line.is_err()) {
        if constexpr (std::is_same_v<Return, Status>) {
            return line.unwrap_err();
        } else {
            static_assert(std::is_constructible_v<Return, ustl::ErrValue<Status>>,
                          "line control must return Status or a status Result");
            return ustl::err(line.unwrap_err());
        }
    }
    return std::invoke(std::forward<Operation>(operation), **line);
}
} // namespace ours::irq

#endif // KERNEL_IRQ_LINE_ACCESS_HPP
