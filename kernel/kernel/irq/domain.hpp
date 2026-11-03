/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KERNEL_IRQ_DOMAIN_HPP
#define KERNEL_IRQ_DOMAIN_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <atomic>

#include <ktl/result.hpp>
#include <ktl/vec.hpp>
#include <ustl/box.hpp>
#include <ustl/option.hpp>
#include <ustl/util/noncopyable.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class Line;
class Registration;
struct ControllerOps;

/// One set of hardware sources, its kernel identities, and its line storage.
/// Construction is transactional. Published domains are destroyed only after
/// revocation, unpublication and the registry's reader grace period.
class Domain final {
  public:
    Domain();
    USTL_NO_MOVEABLE_AND_COPYABLE(Domain);
    ~Domain();

    [[nodiscard]]
    auto init(ustl::views::Span<HIrqNum const> sources, ControllerOps const &controller,
              VIrqNum minimum = 0, void *context = nullptr) -> Status;
    [[nodiscard]]
    auto first_virq() const -> VIrqNum {
        return m_first;
    }
    [[nodiscard]]
    auto num_lines() const -> usize {
        return m_storage.hwirqs.size();
    }
    [[nodiscard]]
    auto contains(VIrqNum const line) const -> bool {
        return line >= m_first && line - m_first < num_lines();
    }
    [[nodiscard]]
    auto hwirq_limit() const -> usize {
        return m_storage.lines.size();
    }
    [[nodiscard]]
    auto virq_of(HIrqNum source) const -> VIrqNum;
    [[nodiscard]]
    auto hwirq_of(VIrqNum line) const -> HIrqNum;

    [[nodiscard]]
    auto begin_removal() -> Status;
    auto finish_removal() -> void;
    [[nodiscard]]
    auto is_destroyed() const -> bool {
        return m_destroyed.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    auto attach(VIrqNum line, Registration &registration) -> Status;
    [[nodiscard]]
    auto attach(VIrqNum line, Registration &registration, ustl::Option<LineMode> mode)
        -> ktl::Result<TriggerMode>;
    [[nodiscard]]
    auto detach(Registration &registration) -> Status;
    auto finalize_oneshot(VIrqNum line, Registration const &registration) -> void;
    [[nodiscard]]
    auto handle_irq(HIrqNum source, Eoi eoi) -> ustl::Option<IrqReturn>;
    [[nodiscard]]
    auto mask(VIrqNum line) -> Status;
    [[nodiscard]]
    auto unmask(VIrqNum line) -> Status;
    [[nodiscard]]
    auto configure(VIrqNum line, TriggerMode trigger, Polarity polarity) -> Status;
    [[nodiscard]]
    auto trigger_mode(VIrqNum line) -> ktl::Result<TriggerMode>;
    [[nodiscard]]
    auto is_masked(VIrqNum line) -> ktl::Result<bool>;
    [[nodiscard]]
    auto spurious_count(VIrqNum line) -> ustl::Option<u64>;
    [[nodiscard]]
    auto revoke_line(VIrqNum line) -> Status;
    auto unmark_dying(VIrqNum line) -> void;
    [[nodiscard]]
    auto is_kernel_owned(VIrqNum line) -> bool;

  private:
    struct Storage {
        ktl::Vec<VIrqNum> virqs;
        ktl::Vec<HIrqNum> hwirqs;
        ktl::Vec<ustl::Box<Line>> lines;
    };

    static constexpr auto VIRQ_ALIGNMENT = u32{32};

    auto irqnum_to_desc(VIrqNum line) -> Line *;
    auto hwirq_to_desc(HIrqNum source) -> Line *;
    [[nodiscard]]
    auto find_line(VIrqNum line) -> ktl::Result<Line *>;

    VIrqNum m_first = 0;
    Storage m_storage;
    std::atomic<bool> m_destroyed{false};
};
} // namespace ours::irq

#endif // KERNEL_IRQ_DOMAIN_HPP
