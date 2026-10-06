/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DESC_TABLE_HPP
#define OURS_IRQ_DESC_TABLE_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class Desc;
class Action;
struct Chip;

/// The line table: a line number to the line's descriptor.
class DescTable {
  public:
    [[nodiscard]]
    auto init(ustl::views::Span<HIrqNum const> hirqnums, Chip const &chip) -> Status;
    auto attach(VIrqNum virqnum, Action &action) -> Status;
    [[nodiscard]]
    auto attach(VIrqNum virqnum, Action &action, ustl::Option<LineMode> mode)
        -> ktl::Result<TriggerMode>;
    auto detach(Action &action) -> Status;
    /// Finalizes `action` on line `virqnum`; nothing if there is no such line.
    auto finalize_oneshot(VIrqNum virqnum, Action const &action) -> void;
    /// Runs the flow of line `virqnum`, ending the interrupt as `eoi` says,
    /// and answers what its actions answered; none if there is no such line,
    /// and so nothing ran and nothing was ended.
    [[nodiscard]]
    auto handle_irq(VIrqNum virqnum, Eoi eoi) -> ustl::Option<IrqReturn>;
    [[nodiscard]]
    auto mask(VIrqNum virqnum) -> Status;
    [[nodiscard]]
    auto unmask(VIrqNum virqnum) -> Status;
    [[nodiscard]]
    auto configure(VIrqNum virqnum, TriggerMode trigger, Polarity polarity) -> Status;
    [[nodiscard]]
    auto trigger_mode(VIrqNum virqnum) -> ktl::Result<TriggerMode>;
    [[nodiscard]]
    auto is_masked(VIrqNum virqnum) -> ktl::Result<bool>;
    [[nodiscard]]
    auto spurious_count(VIrqNum virqnum) -> ustl::Option<u64>;

  private:
    auto irqnum_to_desc(VIrqNum virqnum) -> Desc *;

    /// The line `virqnum` names: InvArgs for INVALID_V_IRQ_NUM, NoResource
    /// if there is no such line.
    [[nodiscard]]
    auto find_line(VIrqNum virqnum) -> ktl::Result<Desc *>;

    ustl::views::Span<Desc *> m_descs;
};
} // namespace ours::irq

#endif // OURS_IRQ_DESC_TABLE_HPP
