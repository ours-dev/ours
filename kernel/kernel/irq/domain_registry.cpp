#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/thread.hpp>

#include <utility>

#include <arch/interrupt.hpp>
#include <heap/new.hpp>
#include <kernel/kernel/irq/domain.hpp>
#include <kernel/kernel/irq/domain_registry.hpp>

namespace ours::irq {
struct DomainRegistry::Entry {
    DomainId const id;
    ustl::Box<Domain> domain;
    DomainEndpoint *const endpoint;
};

DomainRegistry::DomainRegistry() = default;
DomainRegistry::~DomainRegistry() = default;

auto DomainRegistry::claim_locked(DomainId const id, usize *const index) const -> Status {
    auto place = CAPACITY;
    for (usize i = 0; i < CAPACITY; ++i) {
        if (m_slots[i].state == State::Free) {
            if (place == CAPACITY) {
                place = i;
            }
        } else if (m_slots[i].entry->id == id) {
            return Status::AlreadyExists;
        }
    }
    if (place == CAPACITY) {
        return Status::NoSpace;
    }
    *index = place;
    return Status::Ok;
}

/// Allocation stays outside the writer lock. If another publication advances
/// the identity floor, rebuild the unpublished domain from the new floor.
auto DomainRegistry::register_domain(DomainId const id,
                                     ustl::views::Span<HIrqNum const> const sources,
                                     ControllerOps const &controller, DomainEndpoint &endpoint,
                                     void *const context) -> Status {
    if (id == DomainId::Invalid) {
        return Status::InvArgs;
    }
    for (;;) {
        auto floor = VIrqNum{0};
        {
            Guard<sync::SpinLock, IrqSave> const guard{m_lock};
            auto index = usize{0};
            if (auto const status = claim_locked(id, &index); status != Status::Ok) {
                return status;
            }
            floor = m_next_id;
        }
        auto domain = ustl::Box<Domain>::adopt(new (mm::GAF_KERNEL) Domain());
        if (!domain) {
            return Status::NoMemory;
        }
        if (auto const status = domain->init(sources, controller, floor, context);
            status != Status::Ok) {
            return status;
        }
        auto entry =
            ustl::Box<Entry>::adopt(new (mm::GAF_KERNEL) Entry{id, std::move(domain), &endpoint});
        if (!entry) {
            return Status::NoMemory;
        }
        auto status = Status::Ok;
        auto raced = false;
        {
            Guard<sync::SpinLock, IrqSave> const guard{m_lock};
            auto index = usize{0};
            status = claim_locked(id, &index);
            if (status == Status::Ok && endpoint.m_domain.read_with_irqs_off() != nullptr) {
                status = Status::BadState;
            }
            if (status == Status::Ok && m_next_id != floor) {
                raced = true;
            } else if (status == Status::Ok) {
                auto *const published = entry.as_ptr_mut();
                m_next_id = static_cast<VIrqNum>(static_cast<usize>(published->domain->first_id()) +
                                                 published->domain->num_lines());
                m_slots[index].entry = std::move(entry);
                m_slots[index].state = State::Live;
                endpoint.m_domain.publish(published->domain.as_ptr_mut());
                m_published[index].publish(published);
                return Status::Ok;
            }
        }
        if (!raced) {
            return status;
        }
    }
}

auto DomainRegistry::unregister_domain(DomainId const id) -> Status {
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
            if (m_slots[i].state != State::Free && m_slots[i].entry->id == id) {
                if (m_slots[i].state == State::Removing) {
                    return Status::BadState;
                }
                m_slots[i].state = State::Removing;
                index = i;
                entry = m_slots[i].entry.as_ptr_mut();
                break;
            }
        }
    }
    if (entry == nullptr) {
        return Status::NotFound;
    }
    if (auto const status = entry->domain->begin_removal(); status != Status::Ok) {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        m_slots[index].state = State::Live;
        return status;
    }
    entry->domain->finish_removal();
    [[maybe_unused]]
    auto *const from_endpoint = entry->endpoint->m_domain.retract();
    [[maybe_unused]]
    auto *const from_registry = m_published[index].retract();
    DEBUG_ASSERT(from_endpoint == entry->domain.as_ptr_mut() && from_registry == entry);
    task::irq_rcu_synchronize();

    // Remove ownership and release the id together. Destruction outside the
    // lock cannot race another writer inspecting a Removing entry.
    ustl::Box<Entry> retired;
    {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        retired = std::move(m_slots[index].entry);
        m_slots[index].state = State::Free;
    }
    return Status::Ok;
}

auto DomainRegistry::domain_for(VIrqNum const line, task::IrqRcuReadGuard const &guard) const
    -> Domain * {
    for (auto const &publication: m_published) {
        auto *const entry = publication.read(guard);
        if (entry != nullptr && entry->domain->contains(line)) {
            return entry->domain.as_ptr_mut();
        }
    }
    return nullptr;
}

auto DomainRegistry::find(DomainId const id, task::IrqRcuReadGuard const &guard) const -> Domain * {
    for (auto const &publication: m_published) {
        auto *const entry = publication.read(guard);
        if (entry != nullptr && entry->id == id) {
            return entry->domain.as_ptr_mut();
        }
    }
    return nullptr;
}

auto DomainRegistry::irq_for_source(DomainId const id, HIrqNum const source,
                                    task::IrqRcuReadGuard const &guard) const -> VIrqNum {
    auto *const domain = find(id, guard);
    if (domain == nullptr) {
        return VIrqNum::Invalid;
    }
    return domain->irq_for_source(source);
}

auto DomainRegistry::find_line(VIrqNum const line, task::IrqRcuReadGuard const &guard) const
    -> ktl::Result<Line *> {
    if (line == VIrqNum::Invalid) {
        return ustl::err(Status::InvArgs);
    }
    auto *const domain = domain_for(line, guard);
    if (domain == nullptr) {
        return ustl::err(Status::NoResource);
    }
    return domain->find_line(line, guard);
}

auto DomainRegistry::published_domain(DomainEndpoint const &endpoint,
                                      task::IrqRcuReadGuard const &guard) -> Domain * {
    return endpoint.m_domain.read(guard);
}

auto DomainEndpoint::dispatch(HIrqNum const source) const -> ustl::Option<IrqReturn> {
    task::IrqRcuReadGuard const guard;
    auto *const domain = m_domain.read(guard);
    if (domain == nullptr) {
        return ustl::none();
    }
    return domain->handle_irq(source, Eoi::Send);
}
} // namespace ours::irq
