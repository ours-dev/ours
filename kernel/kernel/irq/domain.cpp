#include <ours/irq/domain.hpp>
#include <ours/mm/object_cache.hpp>

#include <algorithm>

#include <heap/new.hpp>
#include <ustl/mem/align.hpp>

namespace ours::irq {
auto Domain::init(ustl::views::Span<HIrqNum const> const hirqnums) -> Status {
    if (!m_virqs.empty()) {
        return Status::BadState;
    }
    if (hirqnums.empty()) {
        return Status::InvArgs;
    }

    auto highest = HIrqNum{0};
    for (auto const hirqnum: hirqnums) {
        if (hirqnum == INVALID_H_IRQ_NUM) {
            return Status::InvArgs;
        }
        highest = std::max(highest, hirqnum);
    }

    auto const nr_hwirqs = static_cast<usize>(highest) + 1;
    auto const first = ustl::round_up(nr_hwirqs, VIRQ_ALIGN);
    // The last virq, and INVALID_V_IRQ_NUM past it, have to fit the number.
    if (first + hirqnums.size() >= static_cast<usize>(INVALID_V_IRQ_NUM)) {
        return Status::InvArgs;
    }

    auto *const raw_virqs = new (mm::GAF_KERNEL) VIrqNum[nr_hwirqs];
    auto *const raw_hwirqs = new (mm::GAF_KERNEL) HIrqNum[hirqnums.size()];
    if (!raw_virqs || !raw_hwirqs) {
        delete[] raw_virqs;
        delete[] raw_hwirqs;
        return Status::OutOfMem;
    }
    auto const virqs = ustl::views::make_span(raw_virqs, nr_hwirqs);
    auto const hwirqs = ustl::views::make_span(raw_hwirqs, hirqnums.size());
    std::ranges::fill(virqs, INVALID_V_IRQ_NUM);

    // First which numbers the domain holds, and whether one is given twice.
    // A held number is marked with 0, which pass two replaces.
    for (auto const hirqnum: hirqnums) {
        if (virqs[hirqnum] != INVALID_V_IRQ_NUM) {
            delete[] raw_virqs;
            delete[] raw_hwirqs;
            return Status::AlreadyExists;
        }
        virqs[hirqnum] = 0;
    }

    // Then the virqs, in the order of the hardware numbers.
    auto next = static_cast<VIrqNum>(first);
    for (auto hirqnum = HIrqNum{0}; hirqnum < nr_hwirqs; ++hirqnum) {
        if (virqs[hirqnum] == INVALID_V_IRQ_NUM) {
            continue;
        }
        virqs[hirqnum] = next;
        hwirqs[next - first] = hirqnum;
        ++next;
    }

    m_first = static_cast<VIrqNum>(first);
    m_virqs = virqs;
    m_hwirqs = hwirqs;
    return Status::Ok;
}

auto Domain::reset() -> void {
    delete[] m_virqs.data();
    delete[] m_hwirqs.data();
    m_first = 0;
    m_virqs = {};
    m_hwirqs = {};
}

auto Domain::virq_of(HIrqNum const hirqnum) const -> VIrqNum {
    if (hirqnum >= m_virqs.size()) {
        return INVALID_V_IRQ_NUM;
    }
    return m_virqs[hirqnum];
}

auto Domain::hwirq_of(VIrqNum const virqnum) const -> HIrqNum {
    if (virqnum < m_first || virqnum - m_first >= m_hwirqs.size()) {
        return INVALID_H_IRQ_NUM;
    }
    return m_hwirqs[virqnum - m_first];
}

auto Domain::hwirq_limit() const -> usize {
    return m_virqs.size();
}
} // namespace ours::irq
