#include <ours/assert.hpp>
#include <ours/irq/domain_registry.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/thread.hpp>

#include <arch/interrupt.hpp>
#include <heap/new.hpp>

namespace ours::irq {
/// What a reader finds: complete before it is published, and never changed,
/// so a reader needs nothing but the pointer.
struct DomainRegistry::Entry {
    DomainId id;
    /// The block of virqs of the table: [first, end).
    VIrqNum first;
    VIrqNum end;
    Domain *table;
    DomainSlot *slot;
};

/// A place is taken from the moment the domain is claimed until it is freed,
/// a removal in progress included, so that the id cannot be registered a
/// second time while the first is on its way out. The ids are kept beside the
/// places, in the writers' own state: an entry is freed by its remover while
/// the place is still Removing, so a writer that looked at one would read freed
/// memory (the review of the first version found exactly that).
auto DomainRegistry::claim_check_locked(DomainId const id, usize *const index) const -> Status {
    auto place = CAPACITY;
    for (usize i = 0; i < CAPACITY; ++i) {
        if (m_state[i] == State::Free) {
            if (place == CAPACITY) {
                place = i;
            }
            continue;
        }
        if (m_id[i] == id) {
            return Status::AlreadyExists;
        }
    }
    if (place == CAPACITY) {
        return Status::NoSpace;
    }
    *index = place;
    return Status::Ok;
}

/// The table is built outside the lock, which only reads and writes a few
/// words, so that it may allocate. It needs the floor for its virqs first, the
/// end of the last block given out, and another registration may move that
/// meanwhile: then the table is built again from the new floor. Two
/// registrations at once are not expected (a platform registers its
/// controllers one after another); the retry is there so that they are right
/// rather than fast.
auto DomainRegistry::register_domain(DomainId const id,
                                     ustl::views::Span<HIrqNum const> const hirqnums,
                                     ControllerOps const &chip, DomainSlot &slot) -> Status {
    if (id == DomainId::Invalid) {
        return Status::InvArgs;
    }

    for (;;) {
        auto floor = VIrqNum{0};
        {
            Guard<sync::SpinLock, IrqSave> const guard{m_lock};
            auto index = usize{0};
            if (auto const status = claim_check_locked(id, &index); status != Status::Ok) {
                return status;
            }
            floor = m_next_virq;
        }

        auto *const table = new (mm::GAF_KERNEL) Domain();
        auto *const entry = new (mm::GAF_KERNEL) Entry{};
        if (table == nullptr || entry == nullptr) {
            delete table;
            delete entry;
            return Status::NoMemory;
        }
        // A refused table is not half kept: Domain::init() gives back what
        // it made.
        if (auto const status = table->init(hirqnums, chip, floor); status != Status::Ok) {
            delete table;
            delete entry;
            return status;
        }
        entry->id = id;
        entry->first = table->first_virq();
        entry->end = static_cast<VIrqNum>(table->first_virq() + table->num_lines());
        entry->table = table;
        entry->slot = &slot;

        auto status = Status::Ok;
        auto raced = false;
        {
            Guard<sync::SpinLock, IrqSave> const guard{m_lock};
            auto index = usize{0};
            status = claim_check_locked(id, &index);
            if (status == Status::Ok && slot.table_with_irqs_off() != nullptr) {
                // Checked in every build: a slot that holds a table would
                // lose it, and its handler would find the new one's lines gone
                // with the first domain.
                status = Status::BadState;
            }
            if (status == Status::Ok && m_next_virq != floor) {
                raced = true;
            } else if (status == Status::Ok) {
                m_state[index] = State::Live;
                m_id[index] = id;
                m_entry[index] = entry;
                m_next_virq = entry->end;
                // Complete before it is published: the release is what lets a
                // reader that finds the entry see the table and the block.
                slot.m_table.publish(table);
                m_published[index].publish(entry);
                return Status::Ok;
            }
        }

        // Never published, so no reader has seen either: they go at once.
        delete table;
        delete entry;
        if (!raced) {
            return status;
        }
    }
}

/// Phase by phase, as the design's "Reclaiming a removed domain" has it. The
/// domain is claimed as Removing first, so that a second remover gets BadState
/// and the id cannot be registered again; then the lines are marked, all or
/// none; then the actions are taken off; then the table is unpublished, which
/// no reader that begins from now on can find; then the grace period, and
/// only after it is anything freed. The block of virqs is not given back.
auto DomainRegistry::unregister_domain(DomainId const id) -> Status {
    // Before anything is changed, and in the release image too: the removal
    // cannot be undone once the lines are revoked, and a call from where it
    // may not wait would return with the readers not waited for, or never.
    ASSERT(!arch::irqs_disabled());
    ASSERT(task::Thread::Current::preemption_state().is_preemption_enabled());
    ASSERT(task::cpu_hotplug_lock().holder() != task::Thread::Current::get());
    if (id == DomainId::Root) {
        return Status::NotSupported;
    }

    auto index = CAPACITY;
    Entry *entry = nullptr;
    {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        for (usize i = 0; i < CAPACITY; ++i) {
            if (m_state[i] != State::Free && m_id[i] == id) {
                if (m_state[i] == State::Removing) {
                    return Status::BadState;
                }
                m_state[i] = State::Removing;
                index = i;
                entry = m_entry[i];
                break;
            }
        }
    }
    if (entry == nullptr) {
        return Status::NotFound;
    }

    if (auto const status = entry->table->begin_removal(); status != Status::Ok) {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        m_state[index] = State::Live;
        return status;
    }
    entry->table->finish_removal();

    // Out of the readers' reach: the slot a root handler holds, and the
    // registry. A reader that began before this still holds the table.
    [[maybe_unused]]
    auto *const from_slot = entry->slot->m_table.retract();
    [[maybe_unused]]
    auto *const from_registry = m_published[index].retract();
    DEBUG_ASSERT(from_slot == entry->table && from_registry == entry);

    task::irq_rcu_synchronize();

    delete entry->table;
    {
        // The place is given back with nothing in it that points at the
        // entry, and only then is the entry freed.
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        m_state[index] = State::Free;
        m_id[index] = DomainId::Invalid;
        m_entry[index] = nullptr;
    }
    delete entry;
    return Status::Ok;
}

auto DomainRegistry::table_of(VIrqNum const virqnum, task::IrqRcuReadGuard const &guard) const
    -> Domain * {
    for (usize i = 0; i < CAPACITY; ++i) {
        auto *const entry = m_published[i].read(guard);
        if (entry != nullptr && virqnum >= entry->first && virqnum < entry->end) {
            return entry->table;
        }
    }
    return nullptr;
}

auto DomainRegistry::find(DomainId const id, task::IrqRcuReadGuard const &guard) const -> Domain * {
    for (usize i = 0; i < CAPACITY; ++i) {
        auto *const entry = m_published[i].read(guard);
        if (entry != nullptr && entry->id == id) {
            return entry->table;
        }
    }
    return nullptr;
}

auto DomainRegistry::virq_of(DomainId const id, HIrqNum const hirqnum,
                             task::IrqRcuReadGuard const &guard) const -> VIrqNum {
    auto *const table = find(id, guard);
    if (table == nullptr) {
        return INVALID_V_IRQ_NUM;
    }
    return table->virq_of(hirqnum);
}
} // namespace ours::irq
