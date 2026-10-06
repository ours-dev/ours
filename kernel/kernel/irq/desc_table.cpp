#include <ours/assert.hpp>
#include <ours/irq/desc.hpp>
#include <ours/irq/desc_table.hpp>
#include <ours/mm/object_cache.hpp>

#include <algorithm>

#include <heap/new.hpp>

namespace ours::irq {
/// The table is indexed by line number and reaches the highest number given.
/// A number in between that names no line keeps a null slot, which
/// irqnum_to_desc() answers like a number past the end: there is no line.
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
///
/// The chip must be able to end an interrupt. A line's flow ends each of its
/// interrupts through the chip, and handle_irq() answering for a line tells
/// arch that the interrupt has been ended, so arch sends no end of its own:
/// a line whose chip had no eoi would leave every interrupt on it in service.
///
/// It must be able to mask a line, too. A line's flow masks the line when an
/// interrupt finds nobody on it, so that a level pin nobody services does not
/// raise the same interrupt again the moment it is ended, for good. Each line
/// takes its first flow from the chip as it is bound (Desc::init()).
auto DescTable::init(ustl::views::Span<HIrqNum const> const hirqnums, Chip const &chip) -> Status {
    if (!m_descs.empty()) {
        return Status::BadState;
    }
    // An empty table would leave m_descs empty too, and so not tell a
    // second call that the table is built.
    if (hirqnums.empty()) {
        return Status::InvArgs;
    }
    if (!chip.issue_eoi || !chip.mask) {
        return Status::InvArgs;
    }

    auto nr_slots = usize{0};
    for (auto const hirqnum: hirqnums) {
        if (hirqnum == INVALID_V_IRQ_NUM) {
            return Status::InvArgs;
        }
        nr_slots = std::max(nr_slots, static_cast<usize>(hirqnum) + 1);
    }

    auto *const raw_descs = new (mm::GAF_KERNEL) Desc *[nr_slots]();
    if (!raw_descs) {
        return Status::OutOfMem;
    }
    auto const descs = ustl::views::make_span(raw_descs, nr_slots);
    // A refused table is not half kept: the lines made so far go with it.
    auto const discard = [descs] {
        for (auto *const desc: descs) {
            delete desc;
        }
        delete[] descs.data();
    };

    for (auto const hirqnum: hirqnums) {
        auto *&slot = descs[hirqnum];
        if (slot) {
            discard();
            return Status::AlreadyExists;
        }
        // A line's number is its hardware number, so nothing maps one to the
        // other: the platform delivers the number it programmed, and that is
        // the line. A second number space, MSI's, is when a map is needed.
        slot = Desc::create(hirqnum, static_cast<VIrqNum>(hirqnum), IrqFlags::None, "IRQD");
        if (!slot) {
            discard();
            return Status::OutOfMem;
        }
        // A line just made has no chip yet, so this cannot answer BadState.
        [[maybe_unused]]
        auto const bound = slot->init(chip);
        DEBUG_ASSERT(Status::Ok == bound);
    }

    m_descs = descs;
    return Status::Ok;
}

auto DescTable::irqnum_to_desc(VIrqNum virqnum) -> Desc * {
    if (virqnum >= m_descs.size()) {
        return nullptr;
    }

    return m_descs[virqnum];
}

auto DescTable::find_line(VIrqNum const virqnum) -> ktl::Result<Desc *> {
    if (virqnum == INVALID_V_IRQ_NUM) {
        return ustl::err(Status::InvArgs);
    }

    auto *const desc = irqnum_to_desc(virqnum);
    if (!desc) {
        return ustl::err(Status::NoResource);
    }
    return ustl::ok(desc);
}

/// Nothing is allocated here: the action is the caller's, so a refused attach
/// leaves nothing behind to free.
auto DescTable::attach(VIrqNum virqnum, Action &action) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->attach(action);
}

auto DescTable::attach(VIrqNum const virqnum, Action &action, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return ustl::err(line.unwrap_err());
    }
    return (*line)->attach(action, mode);
}

/// The action knows its line, so the caller names only the action: on a
/// shared line a number alone cannot say which of its actions to remove.
auto DescTable::detach(Action &action) -> Status {
    auto desc = irqnum_to_desc(action.virqnum());
    if (!desc) {
        return Status::NotFound;
    }

    return desc->detach(action);
}

/// Answers what the line's actions answered. It used to answer Handled
/// whatever happened, even on a line with no action at all, so an
/// interrupt nobody handled could not be told from one somebody did.
///
/// No line is an answer of its own, not None: a line ends its interrupt
/// whatever its actions answer, and a number with no line has nobody to end
/// it, so the caller has to know which of the two it was.
auto DescTable::handle_irq(VIrqNum const virqnum, Eoi const eoi) -> ustl::Option<IrqReturn> {
    auto *const desc = irqnum_to_desc(virqnum);
    if (!desc) {
        return ustl::none();
    }

    return ustl::some(desc->handle_irq(eoi));
}

auto DescTable::mask(VIrqNum const virqnum) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->mask();
}

auto DescTable::unmask(VIrqNum const virqnum) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->unmask();
}

auto DescTable::configure(VIrqNum const virqnum, TriggerMode const trigger, Polarity const polarity)
    -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    return (*line)->configure(trigger, polarity);
}

auto DescTable::trigger_mode(VIrqNum const virqnum) -> ktl::Result<TriggerMode> {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return ustl::err(line.unwrap_err());
    }
    return (*line)->trigger_mode();
}

auto DescTable::spurious_count(VIrqNum virqnum) -> ustl::Option<u64> {
    auto const *const desc = irqnum_to_desc(virqnum);
    if (!desc) {
        return ustl::none();
    }

    return ustl::some(desc->spurious_count());
}

} // namespace ours::irq
