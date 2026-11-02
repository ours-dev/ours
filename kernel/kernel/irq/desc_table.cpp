#include <ours/assert.hpp>
#include <ours/irq/desc.hpp>
#include <ours/irq/desc_table.hpp>
#include <ours/mm/object_cache.hpp>

#include <algorithm>

#include <heap/new.hpp>

namespace ours::irq {
/// The table is indexed by hardware number, the one the interrupt entry
/// delivers, and reaches the highest number given. A number in between that
/// names no line keeps a null slot, which hwirq_to_desc() answers like a
/// number past the end: there is no line. The lines are named by virq, which
/// the domain assigns, one for each hardware number in `hirqnums`. A control
/// call (attach, mask, configure, ...) turns the virq it was given into the
/// hardware number first, in irqnum_to_desc(); the interrupt path has the
/// hardware number already and does not.
///
/// It is built on the boot CPU before any line is unmasked, and handle_irq()
/// reads it without a lock: no interrupt can look at the table while it is
/// written, and it never changes afterwards. The domain is the same.
///
/// Every line is bound to `chip` as it is made, before the table is
/// published, which is what lets mask() and unmask() read a line's chip
/// without a lock. One chip for the whole table is all x86 has today: every
/// line is an IO-APIC pin. MSI, with a chip of its own, is when lines start
/// to differ.
///
/// The chip must be able to end an interrupt. A line's flow ends each of its
/// interrupts through the chip, and handle_irq() answering for a line tells
/// the root handler that the interrupt has been ended, so the handler sends
/// no end of its own: a line whose chip had no eoi would leave every interrupt
/// on it in service.
///
/// It must be able to mask a line, too. A line's flow masks the line when an
/// interrupt finds nobody on it, so that a level pin nobody services does not
/// raise the same interrupt again the moment it is ended, for good. Each line
/// takes its first flow from the chip as it is bound (Desc::init()).
auto DescTable::init(ustl::views::Span<HIrqNum const> const hirqnums, Chip const &chip,
                     VIrqNum const min_first) -> Status {
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

    // The domain numbers the lines. It is also what refuses a number given
    // twice and one that is no hardware number, before anything is made here.
    Domain domain;
    if (auto const status = domain.init(hirqnums, min_first); status != Status::Ok) {
        return status;
    }

    auto const nr_slots = domain.hwirq_limit();
    auto *const raw_descs = new (mm::GAF_KERNEL) Desc *[nr_slots]();
    if (!raw_descs) {
        domain.reset();
        return Status::OutOfMem;
    }
    auto const descs = ustl::views::make_span(raw_descs, nr_slots);
    // A refused table is not half kept: the lines made so far go with it, and
    // so does the domain.
    auto const discard = [descs, &domain] {
        for (auto *const desc: descs) {
            delete desc;
        }
        delete[] descs.data();
        domain.reset();
    };

    for (auto const hirqnum: hirqnums) {
        auto *&slot = descs[hirqnum];
        slot = Desc::create(hirqnum, domain.virq_of(hirqnum), IrqFlags::None, "IRQD");
        if (!slot) {
            discard();
            return Status::OutOfMem;
        }
        // A line just made has no chip yet, so this cannot answer BadState.
        [[maybe_unused]]
        auto const bound = slot->init(chip);
        DEBUG_ASSERT(Status::Ok == bound);
    }

    m_domain = domain;
    m_descs = descs;
    return Status::Ok;
}

/// Two passes, so that a line that cannot be emptied refuses before any other
/// is marked, and then the marks, which undo themselves if a line refuses
/// anyway: an action with no cancel hook can land between the first pass and
/// the mark of its line, and the mark looks again under the line's lock.
auto DescTable::begin_removal() -> Status {
    for (auto *const desc: m_descs) {
        if (desc == nullptr) {
            continue;
        }
        if (auto const status = desc->check_revocable(); status != Status::Ok) {
            return status;
        }
    }
    for (usize i = 0; i < m_descs.size(); ++i) {
        auto *const desc = m_descs[i];
        if (desc == nullptr) {
            continue;
        }
        if (auto const status = desc->mark_dying(); status != Status::Ok) {
            for (usize earlier = 0; earlier < i; ++earlier) {
                if (m_descs[earlier] != nullptr) {
                    m_descs[earlier]->unmark_dying();
                }
            }
            return status;
        }
    }
    return Status::Ok;
}

auto DescTable::finish_removal() -> void {
    for (auto *const desc: m_descs) {
        if (desc != nullptr) {
            desc->revoke();
        }
    }
}

/// The lines are poisoned between their destructor and the heap: a reader that
/// is still on one, which the grace period before this call is there to rule
/// out, then finds its lock and its list nonsense rather than a line that
/// looks fine. The poison is written through a volatile pointer, which the
/// compiler may not drop as a store to memory about to be freed. The table is
/// marked destroyed first, which is what a test reads to tell that a reader
/// was not waited for. A line with an action still on it is a removal that was
/// not finished, and freeing it would leave the action's owner a line that is
/// gone: checked in the release image too, the path being cold.
auto DescTable::destroy() -> void {
    m_destroyed.store(true, std::memory_order_release);
    for (auto *const desc: m_descs) {
        if (desc == nullptr) {
            continue;
        }
        ASSERT(!desc->has_actions(), "a line is destroyed with an action on it");
        desc->~Desc();
        auto *const bytes = reinterpret_cast<volatile u8 *>(desc);
        for (usize i = 0; i < sizeof(Desc); ++i) {
            bytes[i] = 0xDD;
        }
        ::operator delete(desc);
    }
    delete[] m_descs.data();
    m_descs = {};
    m_domain.reset();
}

auto DescTable::virq_of(HIrqNum const hirqnum) const -> VIrqNum {
    return m_domain.virq_of(hirqnum);
}

auto DescTable::hwirq_of(VIrqNum const virqnum) const -> HIrqNum {
    return m_domain.hwirq_of(virqnum);
}

/// The control path. A number that is no virq of the domain, INVALID_V_IRQ_NUM
/// included, becomes INVALID_H_IRQ_NUM, which is past the table.
auto DescTable::irqnum_to_desc(VIrqNum const virqnum) -> Desc * {
    return hwirq_to_desc(m_domain.hwirq_of(virqnum));
}

/// The interrupt path, and the second half of the control path.
auto DescTable::hwirq_to_desc(HIrqNum const hirqnum) -> Desc * {
    if (hirqnum >= m_descs.size()) {
        return nullptr;
    }

    return m_descs[hirqnum];
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

auto DescTable::revoke_line(VIrqNum const virqnum) -> Status {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    if (auto const status = (*line)->mark_dying(); status != Status::Ok) {
        return status;
    }
    (*line)->revoke();
    return Status::Ok;
}

auto DescTable::unmark_dying(VIrqNum const virqnum) -> void {
    if (auto *const desc = irqnum_to_desc(virqnum)) {
        desc->unmark_dying();
    }
}

/// Answers nothing: a line that is not there holds nothing to finalize, and
/// the line checks under its own lock whether the action is still on it.
auto DescTable::finalize_oneshot(VIrqNum const virqnum, Action const &action) -> void {
    auto *const desc = irqnum_to_desc(virqnum);
    if (!desc) {
        return;
    }
    desc->finalize_oneshot(action);
}

/// Answers what the line's actions answered. It used to answer Handled
/// whatever happened, even on a line with no action at all, so an
/// interrupt nobody handled could not be told from one somebody did.
///
/// No line is an answer of its own, not None: a line ends its interrupt
/// whatever its actions answer, and a number with no line has nobody to end
/// it, so the caller has to know which of the two it was.
auto DescTable::handle_irq(HIrqNum const hirqnum, Eoi const eoi) -> ustl::Option<IrqReturn> {
    auto *const desc = hwirq_to_desc(hirqnum);
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

auto DescTable::is_masked(VIrqNum const virqnum) -> ktl::Result<bool> {
    auto const line = find_line(virqnum);
    if (line.is_err()) {
        return ustl::err(line.unwrap_err());
    }
    return (*line)->is_masked();
}

auto DescTable::is_kernel_owned(VIrqNum const virqnum) -> bool {
    auto const *const desc = irqnum_to_desc(virqnum);
    return desc != nullptr && desc->is_kernel_owned();
}

auto DescTable::spurious_count(VIrqNum virqnum) -> ustl::Option<u64> {
    auto const *const desc = irqnum_to_desc(virqnum);
    if (!desc) {
        return ustl::none();
    }

    return ustl::some(desc->spurious_count());
}

} // namespace ours::irq
