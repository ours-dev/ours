/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DOMAIN_HPP
#define OURS_IRQ_DOMAIN_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ustl/views/span.hpp>

namespace ours::irq {
/// Two names for each interrupt line, and the map between them.
///
/// A hardware number (hwirq) is the controller's own number for a pin: on x86
/// the GSI. It is what the platform programs a vector with and what the
/// interrupt entry gets back from the vector, so it is what the interrupt
/// path looks a line up by. A kernel number (virq) is what everything else
/// names the line by: an action, an interrupt object, a user program through
/// `interrupt_create`. The two are never equal: a virq is whatever the domain
/// assigned, and a program that took a GSI for a virq finds no line.
///
/// The virqs of a domain are consecutive, handed out in the order of the
/// hardware numbers, from the first multiple of VIRQ_ALIGN that is past the
/// highest hardware number and not below the floor the caller gives
/// (`min_first`). Being past the highest hardware number is why no virq
/// equals any hwirq of the same domain, whatever the machine: every virq is
/// past all of them. The floor is what keeps the virqs of two domains apart:
/// the caller gives the first virq not yet taken by another domain, and a
/// virq of one domain may equal a hardware number of another.
///
/// Built once, by init(), by one writer before the domain is published, and
/// only read after: the lookups take no lock and are safe from interrupt
/// context. The arrays live until reset(), which a build that failed part-way
/// calls and so does the removal of a domain, after the grace period that
/// waits for its readers.
class Domain {
  public:
    /// The first virq is a multiple of this, so that on a machine with one
    /// IO-APIC (24 pins) it is 32.
    static constexpr auto VIRQ_ALIGN = u32{32};

    /// Builds the domain of the hardware numbers in `hirqnums`, its virqs
    /// starting at or past `min_first` (0 for none: the first domain of a
    /// machine). InvArgs for an empty list, for INVALID_H_IRQ_NUM, or for a
    /// list the virq space cannot hold from there; AlreadyExists if a number
    /// is given twice; BadState if the domain is built already; NoMemory.
    /// Nothing is kept from a call that fails.
    [[nodiscard]]
    auto init(ustl::views::Span<HIrqNum const> hirqnums, VIrqNum min_first = 0) -> Status;

    /// Gives back what init() allocated, so the domain is empty again.
    auto reset() -> void;

    /// The virq of hardware number `hirqnum`; INVALID_V_IRQ_NUM if the
    /// domain holds none.
    [[nodiscard]]
    auto virq_of(HIrqNum hirqnum) const -> VIrqNum;

    /// The hardware number of `virqnum`; INVALID_H_IRQ_NUM if it is not a
    /// virq of this domain.
    [[nodiscard]]
    auto hwirq_of(VIrqNum virqnum) const -> HIrqNum;

    /// One past the highest hardware number the domain holds: the size of a
    /// table indexed by hwirq. 0 until init().
    [[nodiscard]]
    auto hwirq_limit() const -> usize;

    /// The block of virqs the domain holds: the first, and how many. 0 and 0
    /// until init().
    [[nodiscard]]
    auto first_virq() const -> VIrqNum {
        return m_first;
    }
    [[nodiscard]]
    auto num_virqs() const -> usize {
        return m_hwirqs.size();
    }

  private:
    /// The first virq; the virq of the lowest hardware number.
    VIrqNum m_first = 0;
    /// The virq of each hardware number, INVALID_V_IRQ_NUM where the domain
    /// holds none; indexed by hwirq.
    ustl::views::Span<VIrqNum> m_virqs;
    /// The hardware number of each virq; indexed by virq less m_first.
    ustl::views::Span<HIrqNum> m_hwirqs;
};
} // namespace ours::irq

#endif // OURS_IRQ_DOMAIN_HPP
