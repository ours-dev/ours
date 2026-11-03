/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KERNEL_IRQ_DOMAIN_REGISTRY_HPP
#define KERNEL_IRQ_DOMAIN_REGISTRY_HPP 1

#include <ours/irq/domain.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/irq_rcu.hpp>

#include <kernel/kernel/irq/domain.hpp>
#include <ustl/box.hpp>
#include <ustl/util/noncopyable.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class Domain;

/// Private directory and owner of published domains. Readers borrow through
/// an IRQ RCU scope; writers transfer unique ownership outside the lock.
/// Destruction requires all users of the directory to have quiesced.
class DomainRegistry final {
  public:
    static constexpr usize CAPACITY = 8;

    DomainRegistry();
    USTL_NO_MOVEABLE_AND_COPYABLE(DomainRegistry);
    ~DomainRegistry();

    [[nodiscard]]
    auto register_domain(DomainId id, ustl::views::Span<HardwareIrq const> sources,
                         ControllerOps const &controller, DomainEndpoint &endpoint,
                         void *context = nullptr) -> Status;
    [[nodiscard]]
    auto unregister_domain(DomainId id) -> Status;
    [[nodiscard]]
    auto table_of(IrqId line, task::IrqRcuReadGuard const &guard) const -> Domain *;
    [[nodiscard]]
    auto find(DomainId id, task::IrqRcuReadGuard const &guard) const -> Domain *;
    [[nodiscard]]
    auto virq_of(DomainId id, HardwareIrq source, task::IrqRcuReadGuard const &guard) const
        -> IrqId;

    [[nodiscard]]
    auto find_line(IrqId line, task::IrqRcuReadGuard const &guard) const -> ktl::Result<Line *>;

    /// Internal test seam for publication/reclamation checks. Production
    /// controller code can only dispatch through the endpoint.
    [[nodiscard]]
    static auto published_domain(DomainEndpoint const &endpoint, task::IrqRcuReadGuard const &guard)
        -> Domain *;

  private:
    struct Entry;

    enum class State : u8 {
        Free,
        Live,
        Removing,
    };
    struct Slot {
        State state = State::Free;
        ustl::Box<Entry> entry;
    };

    [[nodiscard]]
    TA_REQUIRES(m_lock) auto claim_locked(DomainId id, usize *index) const -> Status;

    sync::SpinLock m_lock;
    Slot m_slots[CAPACITY] TA_GUARDED(m_lock);
    IrqId m_next_virq TA_GUARDED(m_lock) = IrqId{0};
    task::IrqRcuPointer<Entry> m_published[CAPACITY];
};
} // namespace ours::irq

#endif // KERNEL_IRQ_DOMAIN_REGISTRY_HPP
