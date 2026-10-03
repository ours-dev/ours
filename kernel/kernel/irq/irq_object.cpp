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
    if (observers_.size() != 0 && (!is_shared() || !observer.is_shared())) {
        return Status::AlreadyBound;
    }
    if (observers_.size() == 0) {
        irqflags_ = observer.is_shared() ? IrqFlags::Shared : IrqFlags::None;
    }

    observers_.push_back(observer);
    return Status::Ok;
}

auto IrqObject::detach(IrqObserver &observer) -> Status {
    auto _ = observers_.erase(observers_.iterator_to(observer));
    return Status::Ok;
}

auto IrqObject::notify() -> void {
    for (auto &observer: observers_) {
        auto _ = observer.on_event();
    }
}

} // namespace ours::irq
