#include <ours/irq/irq_object.hpp>

#include <heap/new.hpp>

namespace ours::irq {
IrqObject::IrqObject(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name)
    : canary_(this)
    , request_mutex_()
    , irqflags_(flags)
    , irqdata_(hirq, virq, 0 /* No chip available */)
    , observers_()
    , name_(name) {}

auto IrqObject::create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self * {
    return new (mm::GAF_KERNEL) Self(hirq, virq, flags, name);
}

auto IrqObject::init(IrqChip *chip) -> Status {
    irqdata_.chip = chip;
    return Status::Ok;
}

auto IrqObject::attach(IrqObserver &observer) -> Status {
    if (!observer.m_handler) {
        return Status::InvArgs;
    }
    // An observer hangs on one line at a time: its hook has room for one
    // list, and pushing a linked hook again would corrupt both lists.
    if (observer.is_attached()) {
        return Status::BadState;
    }
    if (observers_.size() != 0 && (!is_shared() || !observer.is_shared())) {
        return Status::AlreadyBound;
    }
    if (observers_.size() == 0) {
        irqflags_ = observer.is_shared() ? IrqFlags::Shared : IrqFlags::None;
    }

    observers_.push_back(observer);
    observer.m_virqnum = irqdata_.virqnum;
    return Status::Ok;
}

auto IrqObject::detach(IrqObserver &observer) -> Status {
    // iterator_to() trusts that the observer is in this very list; erasing one
    // that is not would unlink it from whichever list it is on, or from none.
    if (observer.m_virqnum != irqdata_.virqnum) {
        return Status::NotFound;
    }

    auto _ = observers_.erase(observers_.iterator_to(observer));
    observer.m_virqnum = INVALID_V_IRQ_NUM;
    return Status::Ok;
}

auto IrqObject::notify() -> void {
    for (auto &observer: observers_) {
        auto _ = observer.on_event();
    }
}

} // namespace ours::irq
