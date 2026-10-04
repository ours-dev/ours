#include <ours/irq/irq_dispatcher.hpp>
#include <ours/irq/irq_object.hpp>
#include <ours/mm/object_cache.hpp>

#include <heap/new.hpp>

namespace ours::irq {
auto IrqDispatcher::init(usize nr_irqs) -> Status {
    if (nr_irqs > static_cast<usize>(static_cast<VIrqNum>(-1))) {
        return Status::OutOfRange;
    }

    auto raw_objects = new (mm::GAF_KERNEL) IrqObject *[nr_irqs]();
    if (!raw_objects) {
        return Status::OutOfMem;
    }
    objects_ = ustl::views::make_span(raw_objects, nr_irqs);

    for (usize i = 0; i < nr_irqs; ++i) {
        auto const virqnum = static_cast<VIrqNum>(i);
        auto const irqo =
            IrqObject::create(static_cast<HIrqNum>(i), virqnum, IrqFlags::None, "IRQO");
        if (!irqo) {
            return Status::OutOfMem;
        }
        objects_[i] = irqo;
    }
    return Status::Ok;
}

auto IrqDispatcher::irqnum_to_object(VIrqNum virqnum) -> IrqObject * {
    if (virqnum >= objects_.size()) {
        return nullptr;
    }

    return objects_[virqnum];
}

/// Nothing is allocated here: the observer is the caller's, so a refused attach
/// leaves nothing behind to free.
auto IrqDispatcher::attach(VIrqNum virqnum, IrqObserver &observer) -> Status {
    if (virqnum == INVALID_V_IRQ_NUM) {
        return Status::InvArgs;
    }

    auto object = irqnum_to_object(virqnum);
    if (!object) {
        return Status::NoResource;
    }

    return object->attach(observer);
}

/// The observer knows its line, so the caller names only the observer: on a
/// shared line a number alone cannot say which of its observers to remove.
auto IrqDispatcher::detach(IrqObserver &observer) -> Status {
    auto object = irqnum_to_object(observer.virqnum());
    if (!object) {
        return Status::NotFound;
    }

    return object->detach(observer);
}

auto IrqDispatcher::handle_irq(VIrqNum virqnum) -> IrqReturn {
    auto object = irqnum_to_object(virqnum);
    if (!object) {
        return IrqReturn::None;
    }

    object->notify();

    return IrqReturn::Handled;
}

} // namespace ours::irq
