/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DESC_TABLE_HPP
#define OURS_IRQ_DESC_TABLE_HPP 1

#include <ours/irq/domain.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class Desc;
class Action;
struct Chip;

/// The line table: a line's number to the line's descriptor. A line has two
/// numbers, which the table's domain maps to each other: the hardware number
/// the interrupt entry delivers (`HIrqNum`), and the virq (`VIrqNum`) every
/// control call and every action names the line by.
class DescTable {
  public:
    /// Makes a line for each hardware number in `hirqnums` and gives it a
    /// virq (Domain::init()). Refuses, with nothing kept: InvArgs for an
    /// empty list, a number that is no hardware number, or a chip with no
    /// eoi or no mask; AlreadyExists for a number given twice; BadState if
    /// the table is built; OutOfMem.
    [[nodiscard]]
    auto init(ustl::views::Span<HIrqNum const> hirqnums, Chip const &chip) -> Status;
    /// The virq of the line made for hardware number `hirqnum`;
    /// INVALID_V_IRQ_NUM if there is none.
    [[nodiscard]]
    auto virq_of(HIrqNum hirqnum) const -> VIrqNum;
    /// The hardware number of line `virqnum`; INVALID_H_IRQ_NUM if there is
    /// no such line.
    [[nodiscard]]
    auto hwirq_of(VIrqNum virqnum) const -> HIrqNum;
    auto attach(VIrqNum virqnum, Action &action) -> Status;
    [[nodiscard]]
    auto attach(VIrqNum virqnum, Action &action, ustl::Option<LineMode> mode)
        -> ktl::Result<TriggerMode>;
    auto detach(Action &action) -> Status;
    /// Finalizes `action` on line `virqnum`; nothing if there is no such line.
    auto finalize_oneshot(VIrqNum virqnum, Action const &action) -> void;
    /// Runs the flow of the line made for hardware number `hirqnum`, ending
    /// the interrupt as `eoi` says, and answers what its actions answered;
    /// none if there is no such line, and so nothing ran and nothing was
    /// ended. By hardware number, not virq: it is the interrupt path, and the
    /// interrupt entry has the hardware number.
    [[nodiscard]]
    auto handle_irq(HIrqNum hirqnum, Eoi eoi) -> ustl::Option<IrqReturn>;
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
    /// Whether line `virqnum` is held by an action that says Permanent, which
    /// is how the kernel's own code holds a line; false if there is no such
    /// line.
    [[nodiscard]]
    auto is_kernel_owned(VIrqNum virqnum) -> bool;

  private:
    /// The line named by a virq, null if there is none: the control path.
    auto irqnum_to_desc(VIrqNum virqnum) -> Desc *;

    /// The line made for a hardware number, null if there is none: the
    /// interrupt path.
    auto hwirq_to_desc(HIrqNum hirqnum) -> Desc *;

    /// The line `virqnum` names: InvArgs for INVALID_V_IRQ_NUM, NoResource
    /// if there is no such line.
    [[nodiscard]]
    auto find_line(VIrqNum virqnum) -> ktl::Result<Desc *>;

    Domain m_domain;
    /// The lines, indexed by hardware number.
    ustl::views::Span<Desc *> m_descs;
};
} // namespace ours::irq

#endif // OURS_IRQ_DESC_TABLE_HPP
