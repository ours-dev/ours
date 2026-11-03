/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DESC_TABLE_HPP
#define OURS_IRQ_DESC_TABLE_HPP 1

#include <ours/irq/domain.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <atomic>

#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class Line;
class Registration;
struct ControllerOps;

/// The line table: a line's number to the line's descriptor. A line has two
/// numbers, which the table's domain maps to each other: the hardware number
/// the interrupt entry delivers (`HIrqNum`), and the virq (`VIrqNum`) every
/// control call and every action names the line by.
class DescTable {
  public:
    /// Makes a line for each hardware number in `hirqnums` and gives it a
    /// virq (Domain::init()), the virqs from `min_first` up: 0 for none, the
    /// first table of a machine; the end of the last block given out for a
    /// table that is not the first. Refuses, with nothing kept: InvArgs for an
    /// empty list, a number that is no hardware number, a chip with no eoi or
    /// no mask, or a block that does not fit the virq space; AlreadyExists for
    /// a number given twice; BadState if the table is built; OutOfMem.
    [[nodiscard]]
    auto init(ustl::views::Span<HIrqNum const> hirqnums, ControllerOps const &chip,
              VIrqNum min_first = 0) -> Status;

    /// The block of virqs the table's lines hold, [first_virq(), first_virq() +
    /// num_lines()). The floor `init()` was given is where the block starts
    /// or lies above, so that two tables of a machine hold disjoint blocks.
    [[nodiscard]]
    auto first_virq() const -> VIrqNum {
        return m_domain.first_virq();
    }
    [[nodiscard]]
    auto num_lines() const -> usize {
        return m_domain.num_virqs();
    }

    /// The removal of the table's domain, over every line, in the steps
    /// Line documents. begin_removal() refuses before anything is marked
    /// (check_revocable() over every line) and then marks every line
    /// (mark_dying()), undoing the marks it made if one refuses; it answers
    /// the first refusal, AlreadyBound, with nothing changed but a short
    /// window in which attach(), unmask() and configure() on the lines marked
    /// answered NoResource. It takes no line to be marked already, as
    /// revoke_line() leaves its line. finish_removal(), only after
    /// begin_removal() answered Ok, then takes every action off every line
    /// (revoke()). All thread context, and a table is removed by one caller at
    /// a time.
    [[nodiscard]]
    auto begin_removal() -> Status;
    auto finish_removal() -> void;

    /// Frees the lines and the domain's arrays of a table whose removal is
    /// finished (no line has an action: asserted) and whose readers are gone
    /// (irq_rcu_synchronize() has returned since it was unpublished). The
    /// lines are poisoned, so a reader that is still on one finds its lock and
    /// its list nonsense rather than a line that looks fine. The table is empty
    /// afterwards and may be freed.
    auto destroy() -> void;

    /// Whether destroy() has begun. A debug sentinel for tests and asserts: a
    /// reader that can read it true was not waited for.
    [[nodiscard]]
    auto is_destroyed() const -> bool {
        return m_destroyed.load(std::memory_order_acquire);
    }
    /// The virq of the line made for hardware number `hirqnum`;
    /// INVALID_V_IRQ_NUM if there is none.
    [[nodiscard]]
    auto virq_of(HIrqNum hirqnum) const -> VIrqNum;
    /// The hardware number of line `virqnum`; INVALID_H_IRQ_NUM if there is
    /// no such line.
    [[nodiscard]]
    auto hwirq_of(VIrqNum virqnum) const -> HIrqNum;
    auto attach(VIrqNum virqnum, Registration &action) -> Status;
    [[nodiscard]]
    auto attach(VIrqNum virqnum, Registration &action, ustl::Option<LineMode> mode)
        -> ktl::Result<TriggerMode>;
    auto detach(Registration &action) -> Status;
    /// Finalizes `action` on line `virqnum`; nothing if there is no such line.
    auto finalize_oneshot(VIrqNum virqnum, Registration const &action) -> void;
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
    /// Takes every action off line `virqnum`, as the removal of the domain it
    /// belongs to would (Line::mark_dying(), then Line::revoke()): the line
    /// is left marked, and its actions told. NoResource if there is no such
    /// line; AlreadyBound, with nothing changed, if the line is Permanent or
    /// an action on it gave no cancel hook.
    [[nodiscard]]
    auto revoke_line(VIrqNum virqnum) -> Status;

    /// Takes the mark off line `virqnum`, so that it takes actions again. For
    /// a removal that was refused further on, and for tests: a domain that is
    /// gone has no line to mark. Nothing if there is no such line.
    auto unmark_dying(VIrqNum virqnum) -> void;

    /// Whether line `virqnum` is held by an action that says Permanent, which
    /// is how the kernel's own code holds a line; false if there is no such
    /// line.
    [[nodiscard]]
    auto is_kernel_owned(VIrqNum virqnum) -> bool;

  private:
    /// The line named by a virq, null if there is none: the control path.
    auto irqnum_to_desc(VIrqNum virqnum) -> Line *;

    /// The line made for a hardware number, null if there is none: the
    /// interrupt path.
    auto hwirq_to_desc(HIrqNum hirqnum) -> Line *;

    /// The line `virqnum` names: InvArgs for INVALID_V_IRQ_NUM, NoResource
    /// if there is no such line.
    [[nodiscard]]
    auto find_line(VIrqNum virqnum) -> ktl::Result<Line *>;

    Domain m_domain;
    /// The lines, indexed by hardware number.
    ustl::views::Span<Line *> m_descs;
    std::atomic<bool> m_destroyed{false};
};
} // namespace ours::irq

#endif // OURS_IRQ_DESC_TABLE_HPP
