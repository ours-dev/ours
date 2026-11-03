/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KERNEL_IRQ_DOMAIN_HPP
#define KERNEL_IRQ_DOMAIN_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/task/irq_rcu.hpp>

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
              VIrqNum minimum = VIrqNum{0}, void *context = nullptr) -> Status;
    [[nodiscard]]
    auto first_id() const -> VIrqNum {
        return m_first;
    }
    [[nodiscard]]
    auto num_lines() const -> usize {
        return m_storage.sources.size();
    }
    [[nodiscard]]
    auto contains(VIrqNum const line) const -> bool {
        return line >= m_first && static_cast<u32>(line) - static_cast<u32>(m_first) < num_lines();
    }
    [[nodiscard]]
    auto source_limit() const -> usize {
        return m_storage.lines.size();
    }
    [[nodiscard]]
    auto irq_for_source(HIrqNum source) const -> VIrqNum;
    [[nodiscard]]
    auto source_for_irq(VIrqNum line) const -> HIrqNum;

    [[nodiscard]]
    auto begin_removal() -> Status;
    auto finish_removal() -> void;
    [[nodiscard]]
    auto is_destroyed() const -> bool {
        return m_destroyed.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    auto detach(Registration &registration) -> Status;
    auto finalize_oneshot(VIrqNum line, Registration const &registration) -> void;
    [[nodiscard]]
    auto handle_irq(HIrqNum source, Eoi eoi) -> ustl::Option<IrqReturn>;
    [[nodiscard]]
    auto spurious_count(VIrqNum line) -> ustl::Option<u64>;
    [[nodiscard]]
    auto revoke_line(VIrqNum line) -> Status;
    auto unmark_dying(VIrqNum line) -> void;
    [[nodiscard]]
    auto is_kernel_owned(VIrqNum line) -> bool;

    /// Internal borrow valid only inside the supplied reader scope.
    [[nodiscard]]
    auto find_line(VIrqNum line, task::IrqRcuReadGuard const &guard) -> ktl::Result<Line *>;

  private:
    struct Storage {
        ktl::Vec<VIrqNum> ids;
        ktl::Vec<HIrqNum> sources;
        ktl::Vec<ustl::Box<Line>> lines;
    };

    static constexpr auto VIRQ_ALIGNMENT = u32{32};

    auto line_by_id(VIrqNum line) -> Line *;
    auto line_by_source(HIrqNum source) -> Line *;

    VIrqNum m_first = VIrqNum{0};
    Storage m_storage;
    std::atomic<bool> m_destroyed{false};
};
} // namespace ours::irq

#endif // KERNEL_IRQ_DOMAIN_HPP
