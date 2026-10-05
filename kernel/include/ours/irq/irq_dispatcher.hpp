/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_DISPATCHER_HPP
#define OURS_IRQ_IRQ_DISPATCHER_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class IrqObject;
class IrqObserver;
struct IrqChip;

class IrqDispatcher {
  public:
    [[nodiscard]]
    auto init(ustl::views::Span<HIrqNum const> hirqnums, IrqChip const &chip) -> Status;
    auto attach(VIrqNum virqnum, IrqObserver &observer) -> Status;
    auto detach(IrqObserver &observer) -> Status;
    auto handle_irq(VIrqNum virqnum) -> IrqReturn;
    [[nodiscard]]
    auto mask(VIrqNum virqnum) -> Status;
    [[nodiscard]]
    auto unmask(VIrqNum virqnum) -> Status;
    [[nodiscard]]
    auto configure(VIrqNum virqnum, TriggerMode trigger, Polarity polarity) -> Status;
    [[nodiscard]]
    auto spurious_count(VIrqNum virqnum) -> ustl::Option<u64>;

  private:
    auto irqnum_to_object(VIrqNum virqnum) -> IrqObject *;

    /// The line `virqnum` names: InvArgs for INVALID_V_IRQ_NUM, NoResource
    /// if there is no such line.
    [[nodiscard]]
    auto find_line(VIrqNum virqnum) -> ktl::Result<IrqObject *>;

    ustl::views::Span<IrqObject *> objects_;
};
} // namespace ours::irq

#endif // OURS_IRQ_IRQ_DISPATCHER_HPP
