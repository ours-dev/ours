/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DOMAIN_REGISTRY_HPP
#define OURS_IRQ_DOMAIN_REGISTRY_HPP 1

#include <ours/irq/chip.hpp>
#include <ours/irq/desc_table.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/irq_rcu.hpp>

#include <ustl/util/noncopyable.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
/// Names an interrupt domain: a controller's table of lines. A number the
/// registrant of the domain chooses, by the rule of the platform that has more
/// than one: the root controller is Root on every platform (D12 of
/// `.agents/tasks/irq-multi-arch/DESIGN.md`). Not a name that outlives the
/// domain: the id of a removed domain may be registered again, for a
/// controller that is not the one it named, and only a virq is never reused.
enum class DomainId : u32 {
    Root = 0,
    Invalid = 0xFFFF'FFFF,
};

/// Where a controller's root handler finds the table of its domain without
/// asking the registry: the registrant owns the slot, which has static
/// storage, and the registry publishes the table into it when the domain is
/// registered and takes it out when it is removed. The handler runs with
/// interrupts off, so reads it with `table_with_irqs_off()`, and a null
/// answer means the domain is gone: there is no line, and the interrupt is
/// ended by the handler (the path that exists for a vector no line has).
class DomainSlot final {
  public:
    DomainSlot() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(DomainSlot);
    ~DomainSlot() = default;

    [[nodiscard]]
    auto table_with_irqs_off() const -> DescTable * {
        return m_table.read_with_irqs_off();
    }

  private:
    friend class DomainRegistry;

    task::IrqRcuPointer<DescTable> m_table;
};

/// The interrupt controllers' domains of a machine: for each, an id, the block
/// of virqs it owns, and its table of lines. A control call has a virq and
/// asks which table it belongs to (table_of()); interrupt_map has a domain and
/// a hardware number and asks which virq that is (virq_of()). The interrupt
/// path does not ask: a root handler holds its domain's DomainSlot.
///
/// Domains may be registered and removed after boot (Root stays). The readers are
/// task::IrqRcuReadGuard sections, with no lock, and the answer is good only
/// inside the section; a writer unpublishes first and frees only after
/// task::irq_rcu_synchronize() ("Reclaiming a removed domain" in the design).
/// The writers' lock is a spin lock that nothing else is ever held with, and
/// only for a few stores: a table is built and freed outside it.
///
/// The virqs are globally unique: a domain's block starts at or above the end
/// of the last one given out, and the mark is never lowered, so the block of a
/// removed domain stays dead and a stale virq answers "no line", never
/// another's. A fixed number of domains at once.
class DomainRegistry final {
  public:
    static constexpr usize CAPACITY = 8;

    DomainRegistry() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(DomainRegistry);
    ~DomainRegistry() = default;

    /// Builds the domain's table over the hardware numbers in `hirqnums`, each
    /// line bound to `chip`, and publishes it under `id` and into `slot`.
    /// Thread context, and may allocate. InvArgs for Invalid, for an empty list
    /// or a number that is no hardware number or a chip that cannot end an
    /// interrupt or mask (DescTable::init()); AlreadyExists for an id in use,
    /// also while it is being removed, or a number given twice; BadState for
    /// a slot that holds a table; NoSpace when CAPACITY domains are registered;
    /// NoMemory. Nothing is kept from a refusal. The slot must not be freed
    /// while the domain is registered or any handler holds it, and the chip
    /// must outlive the domain.
    [[nodiscard]]
    auto register_domain(DomainId id, ustl::views::Span<HIrqNum const> hirqnums, Chip const &chip,
                         DomainSlot &slot) -> Status;

    /// Removes the domain: marks every line (a Permanent line, or an action
    /// with no cancel hook, refuses with AlreadyBound and nothing changes but
    /// a window in which attach(), unmask() and configure() on the lines
    /// marked answered NoResource), takes every action off every line and
    /// tells it, unpublishes the table from the registry and the slot, waits
    /// for the readers, and frees it. Thread context, interrupts and preemption
    /// on, no spin lock held, not under the CPU hotplug lock
    /// (task::irq_rcu_synchronize()), and it may wait as long as the longest
    /// read with interrupts off.
    ///
    /// NotFound for an id not registered, NotSupported for Root (the root
    /// handler is installed once and cannot be taken out), BadState while the
    /// domain is being removed by another caller (which may then be refused
    /// and leave it registered: look again with find()). The contexts above are
    /// asserted at the top, before anything is changed, in the release image
    /// too.
    ///
    /// Before it is called the registrant has stopped whatever delivers into the
    /// slot (detached its action on the parent line, masked the parent), and
    /// frees the controller only after it returns: the grace period covers
    /// readers of the table and the lines, and nothing else.
    [[nodiscard]]
    auto unregister_domain(DomainId id) -> Status;

    /// The table of the domain that owns `virqnum`, or null if none does, for
    /// as long as `guard` lives and no longer.
    [[nodiscard]]
    auto table_of(VIrqNum virqnum, task::IrqRcuReadGuard const &guard) const -> DescTable *;

    /// The table of domain `id`, or null.
    [[nodiscard]]
    auto find(DomainId id, task::IrqRcuReadGuard const &guard) const -> DescTable *;

    /// The virq of hardware number `hirqnum` in domain `id`;
    /// INVALID_V_IRQ_NUM if there is no such domain or the domain has no such
    /// line. The number is good for good: virqs are not reused, so it is safe
    /// to keep after `guard` ends.
    [[nodiscard]]
    auto virq_of(DomainId id, HIrqNum hirqnum, task::IrqRcuReadGuard const &guard) const -> VIrqNum;

  private:
    struct Entry;

    enum class State : u8 {
        Free,
        Live,
        Removing,
    };

    /// With the lock held: whether `id` can be registered now, and the index of
    /// a free place for it if so. AlreadyExists, NoSpace, or Ok. It reads the
    /// ids kept here and never an entry: an entry is freed by the remover while
    /// its place is still taken.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto claim_check_locked(DomainId id, usize *index) const -> Status;

    sync::SpinLock m_lock;
    /// The writers' view of each place, and the end of the last block of virqs
    /// given out.
    State m_state[CAPACITY] TA_GUARDED(m_lock){};
    DomainId m_id[CAPACITY] TA_GUARDED(m_lock){};
    Entry *m_entry[CAPACITY] TA_GUARDED(m_lock){};
    VIrqNum m_next_virq TA_GUARDED(m_lock) = 0;
    /// What the readers see: an entry, once it is complete, until the domain is
    /// being freed.
    task::IrqRcuPointer<Entry> m_published[CAPACITY];
};
} // namespace ours::irq

#endif // #ifndef OURS_IRQ_DOMAIN_REGISTRY_HPP
