#include <ktl/new.hpp>
#include <ours/irq/irq_dispatcher.hpp>
#include <ours/irq/irq_object.hpp>
#include <ours/mm/object_cache.hpp>

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

auto IrqDispatcher::request_irq(VIrqNum irqnum, IrqHandler handler, IrqFlags flags,
                                char const *name) -> Status {
    if (irqnum == INVALID_V_IRQ_NUM) {
        return Status::InvalidArguments;
    }
    if (!handler) {
        return Status::InvalidArguments;
    }

    auto object = irqnum_to_object(irqnum);
    if (!object) {
        return Status::NoResource;
    }

    auto observer = new (mm::GAF_KERNEL) IrqObserver(irqnum, flags, handler, name);
    if (!observer) {
        return Status::OutOfMem;
    }
    return object->attach(*observer);
}

auto IrqDispatcher::release_irq(VIrqNum virqnum) -> Status {
    return Status::Unimplemented;
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
