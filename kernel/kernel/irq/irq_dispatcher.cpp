#include <ours/assert.hpp>
#include <ours/irq/irq_dispatcher.hpp>
#include <ours/irq/irq_object.hpp>
#include <ours/mm/object_cache.hpp>

#include <algorithm>

#include <heap/new.hpp>

namespace ours::irq {
/// The table is indexed by line number and reaches the highest number given.
/// A number in between that names no line keeps a null slot, which
/// irqnum_to_object() answers like a number past the end: there is no line.
///
/// It is built on the boot CPU before any line is unmasked, and handle_irq()
/// reads it without a lock: no interrupt can look at the table while it is
/// written, and it never changes afterwards.
///
/// Every line is bound to `chip` as it is made, before the table is
/// published, which is what lets mask() and unmask() read a line's chip
/// without a lock. One chip for the whole table is all x86 has today: every
/// line is an IO-APIC pin. MSI, with a chip of its own, is when lines start
/// to differ.
auto IrqDispatcher::init(ustl::views::Span<HIrqNum const> const hirqnums, IrqChip const &chip)
    -> Status {
    if (!objects_.empty()) {
        return Status::BadState;
    }
    // An empty table would leave objects_ empty too, and so not tell a
    // second call that the table is built.
    if (hirqnums.empty()) {
        return Status::InvArgs;
    }

    auto nr_slots = usize{0};
    for (auto const hirqnum: hirqnums) {
        if (hirqnum == INVALID_V_IRQ_NUM) {
            return Status::InvArgs;
        }
        nr_slots = std::max(nr_slots, static_cast<usize>(hirqnum) + 1);
    }

    auto *const raw_objects = new (mm::GAF_KERNEL) IrqObject *[nr_slots]();
    if (!raw_objects) {
        return Status::OutOfMem;
    }
    auto const objects = ustl::views::make_span(raw_objects, nr_slots);
    // A refused table is not half kept: the lines made so far go with it.
    auto const discard = [objects] {
        for (auto *const object: objects) {
            delete object;
        }
        delete[] objects.data();
    };

    for (auto const hirqnum: hirqnums) {
        auto *&slot = objects[hirqnum];
        if (slot) {
            discard();
            return Status::AlreadyExists;
        }
        // A line's number is its hardware number, so nothing maps one to the
        // other: the platform delivers the number it programmed, and that is
        // the line. A second number space, MSI's, is when a map is needed.
        slot = IrqObject::create(hirqnum, static_cast<VIrqNum>(hirqnum), IrqFlags::None, "IRQO");
        if (!slot) {
            discard();
            return Status::OutOfMem;
        }
        // A line just made has no chip yet, so this cannot answer BadState.
        [[maybe_unused]]
        auto const bound = slot->init(chip);
        DEBUG_ASSERT(Status::Ok == bound);
    }

    objects_ = objects;
    return Status::Ok;
}

auto IrqDispatcher::irqnum_to_object(VIrqNum virqnum) -> IrqObject * {
    if (virqnum >= objects_.size()) {
        return nullptr;
    }

    return objects_[virqnum];
}

auto IrqDispatcher::find_line(VIrqNum const virqnum) -> ktl::Result<IrqObject *> {
    if (virqnum == INVALID_V_IRQ_NUM) {
        return ustl::err(Status::InvArgs);
    }

    auto *const object = irqnum_to_object(virqnum);
    if (!object) {
        return ustl::err(Status::NoResource);
    }
    return ustl::ok(object);
}

/// Nothing is allocated here: the observer is the caller's, so a refused attach
/// leaves nothing behind to free.
auto IrqDispatcher::attach(VIrqNum virqnum, IrqObserver &observer) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->attach(observer);
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

/// Answers what the line's observers answered. It used to answer Handled
/// whatever happened, even on a line with no observer at all, so an
/// interrupt nobody handled could not be told from one somebody did.
auto IrqDispatcher::handle_irq(VIrqNum virqnum) -> IrqReturn {
    auto object = irqnum_to_object(virqnum);
    if (!object) {
        return IrqReturn::None;
    }

    return object->notify();
}

auto IrqDispatcher::mask(VIrqNum const virqnum) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->mask();
}

auto IrqDispatcher::unmask(VIrqNum const virqnum) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->unmask();
}

auto IrqDispatcher::configure(VIrqNum const virqnum, TriggerMode const trigger,
                              Polarity const polarity) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->configure(trigger, polarity);
}

auto IrqDispatcher::spurious_count(VIrqNum virqnum) -> ustl::Option<u64> {
    auto const *const object = irqnum_to_object(virqnum);
    if (!object) {
        return ustl::none();
    }

    return ustl::some(object->spurious_count());
}

} // namespace ours::irq
