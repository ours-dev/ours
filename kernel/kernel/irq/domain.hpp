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
    auto init(ustl::views::Span<HardwareIrq const> sources, ControllerOps const &controller,
              IrqId minimum = IrqId{0}, void *context = nullptr) -> Status;
    [[nodiscard]]
    auto first_id() const -> IrqId {
        return m_first;
    }
    [[nodiscard]]
    auto num_lines() const -> usize {
        return m_storage.sources.size();
    }
    [[nodiscard]]
    auto contains(IrqId const line) const -> bool {
        return line >= m_first && static_cast<u32>(line) - static_cast<u32>(m_first) < num_lines();
    }
    [[nodiscard]]
    auto source_limit() const -> usize {
        return m_storage.lines.size();
    }
    [[nodiscard]]
    auto irq_for_source(HardwareIrq source) const -> IrqId;
    [[nodiscard]]
    auto source_for_irq(IrqId line) const -> HardwareIrq;

    [[nodiscard]]
    auto begin_removal() -> Status;
    auto finish_removal() -> void;
    [[nodiscard]]
    auto is_destroyed() const -> bool {
        return m_destroyed.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    auto detach(Registration &registration) -> Status;
    auto finalize_oneshot(IrqId line, Registration const &registration) -> void;
    [[nodiscard]]
    auto handle_irq(HardwareIrq source, Eoi eoi) -> ustl::Option<IrqReturn>;
    [[nodiscard]]
    auto spurious_count(IrqId line) -> ustl::Option<u64>;
    [[nodiscard]]
    auto revoke_line(IrqId line) -> Status;
    auto unmark_dying(IrqId line) -> void;
    [[nodiscard]]
    auto is_kernel_owned(IrqId line) -> bool;

    /// Internal borrow valid only inside the supplied reader scope.
    [[nodiscard]]
    auto find_line(IrqId line, task::IrqRcuReadGuard const &guard) -> ktl::Result<Line *>;

  private:
    struct Storage {
        ktl::Vec<IrqId> ids;
        ktl::Vec<HardwareIrq> sources;
        ktl::Vec<ustl::Box<Line>> lines;
    };

    static constexpr auto VIRQ_ALIGNMENT = u32{32};

    auto line_by_id(IrqId line) -> Line *;
    auto line_by_source(HardwareIrq source) -> Line *;

    IrqId m_first = IrqId{0};
    Storage m_storage;
    std::atomic<bool> m_destroyed{false};
};
} // namespace ours::irq

#endif // KERNEL_IRQ_DOMAIN_HPP
