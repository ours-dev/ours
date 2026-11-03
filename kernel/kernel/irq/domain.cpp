#include <ours/assert.hpp>
#include <ours/irq/line.hpp>
#include <ours/mm/gaf.hpp>

#include <algorithm>
#include <utility>

#include <heap/new.hpp>
#include <kernel/kernel/irq/domain.hpp>
#include <ustl/mem/align.hpp>

namespace ours::irq {
Domain::Domain() = default;

Domain::~Domain() {
    m_destroyed.store(true, std::memory_order_release);
    for (auto &line: m_storage.lines) {
        if (line) {
            ASSERT(!line->has_registrations(), "a domain is destroyed with a live registration");
        }
    }
}

/// Build all storage off to the side. Each Box releases its line on every
/// refused build, and publication cannot observe a partial source set.
auto Domain::init(ustl::views::Span<HardwareIrq const> const sources,
                  ControllerOps const &controller, IrqId const minimum, void *const context)
    -> Status {
    if (!m_storage.lines.empty()) {
        return Status::BadState;
    }
    if (sources.empty() || !controller.is_valid()) {
        return Status::InvArgs;
    }
    auto highest = HardwareIrq{0};
    for (auto const source: sources) {
        if (source == HardwareIrq::Invalid) {
            return Status::InvArgs;
        }
        highest = std::max(highest, source);
    }
    auto const slots = static_cast<usize>(highest) + 1;
    auto const first = ustl::round_up(std::max(slots, static_cast<usize>(minimum)), VIRQ_ALIGNMENT);
    if (first + sources.size() >= static_cast<usize>(IrqId::Invalid)) {
        return Status::InvArgs;
    }

    Storage storage;
    if (storage.virqs.resize(slots).is_err() || storage.hwirqs.resize(sources.size()).is_err() ||
        storage.lines.resize(slots).is_err()) {
        return Status::OutOfMem;
    }
    std::ranges::fill(storage.virqs, IrqId::Invalid);
    for (auto const source: sources) {
        auto const index = static_cast<usize>(source);
        if (storage.virqs[index] != IrqId::Invalid) {
            return Status::AlreadyExists;
        }
        storage.virqs[index] = IrqId{0};
    }
    auto next = first;
    for (usize index = 0; index < slots; ++index) {
        if (storage.virqs[index] == IrqId::Invalid) {
            continue;
        }
        auto const source = static_cast<HardwareIrq>(index);
        auto const id = static_cast<IrqId>(next);
        storage.virqs[index] = id;
        storage.hwirqs[next - first] = source;
        auto line = ustl::Box<Line>::adopt(Line::create(source, id, IrqFlags::None, "irq"));
        if (!line) {
            return Status::OutOfMem;
        }
        if (auto const status = line->init(controller, context); status != Status::Ok) {
            return status;
        }
        storage.lines[index] = std::move(line);
        ++next;
    }
    m_first = static_cast<IrqId>(first);
    m_storage = std::move(storage);
    return Status::Ok;
}

/// The second pass rechecks under each line's lock, so a registration that
/// races the initial check either refuses removal or sees the dying mark.
auto Domain::begin_removal() -> Status {
    for (auto &line: m_storage.lines) {
        if (line) {
            if (auto const status = line->check_revocable(); status != Status::Ok) {
                return status;
            }
        }
    }
    for (usize index = 0; index < m_storage.lines.size(); ++index) {
        auto &line = m_storage.lines[index];
        if (!line) {
            continue;
        }
        if (auto const status = line->mark_dying(); status != Status::Ok) {
            for (usize earlier = 0; earlier < index; ++earlier) {
                if (m_storage.lines[earlier]) {
                    m_storage.lines[earlier]->unmark_dying();
                }
            }
            return status;
        }
    }
    return Status::Ok;
}

auto Domain::finish_removal() -> void {
    for (auto &line: m_storage.lines) {
        if (line) {
            line->revoke();
        }
    }
}

auto Domain::virq_of(HardwareIrq const hirqnum) const -> IrqId {
    if (static_cast<usize>(hirqnum) >= m_storage.virqs.size()) {
        return IrqId::Invalid;
    }
    return m_storage.virqs[static_cast<usize>(hirqnum)];
}

auto Domain::hwirq_of(IrqId const virqnum) const -> HardwareIrq {
    if (virqnum < m_first ||
        static_cast<u32>(virqnum) - static_cast<u32>(m_first) >= m_storage.hwirqs.size()) {
        return HardwareIrq::Invalid;
    }
    return m_storage.hwirqs[static_cast<u32>(virqnum) - static_cast<u32>(m_first)];
}

/// The control path. A number that is no virq of the domain, IrqId::Invalid
/// included, becomes HardwareIrq::Invalid, which is past the table.
auto Domain::line_by_id(IrqId const virqnum) -> Line * {
    return line_by_source(hwirq_of(virqnum));
}

/// The interrupt path, and the second half of the control path.
auto Domain::line_by_source(HardwareIrq const hirqnum) -> Line * {
    if (static_cast<usize>(hirqnum) >= m_storage.lines.size()) {
        return nullptr;
    }

    return m_storage.lines[static_cast<usize>(hirqnum)].as_ptr_mut();
}

auto Domain::find_line(IrqId const virqnum, task::IrqRcuReadGuard const &) -> ktl::Result<Line *> {
    if (virqnum == IrqId::Invalid) {
        return ustl::err(Status::InvArgs);
    }

    auto *const line = line_by_id(virqnum);
    if (!line) {
        return ustl::err(Status::NoResource);
    }
    return ustl::ok(line);
}

/// The action knows its line, so the caller names only the action: on a
/// shared line a number alone cannot say which of its actions to remove.
auto Domain::detach(Registration &action) -> Status {
    auto line = line_by_id(action.virqnum());
    if (!line) {
        return Status::NotFound;
    }

    return line->detach(action);
}

auto Domain::revoke_line(IrqId const virqnum) -> Status {
    task::IrqRcuReadGuard const guard;
    auto const line = find_line(virqnum, guard);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    if (auto const status = (*line)->mark_dying(); status != Status::Ok) {
        return status;
    }
    (*line)->revoke();
    return Status::Ok;
}

auto Domain::unmark_dying(IrqId const virqnum) -> void {
    if (auto *const line = line_by_id(virqnum)) {
        line->unmark_dying();
    }
}

/// Answers nothing: a line that is not there holds nothing to finalize, and
/// the line checks under its own lock whether the action is still on it.
auto Domain::finalize_oneshot(IrqId const virqnum, Registration const &action) -> void {
    auto *const line = line_by_id(virqnum);
    if (!line) {
        return;
    }
    line->finalize_oneshot(action);
}

/// Answers what the line's actions answered. It used to answer Handled
/// whatever happened, even on a line with no action at all, so an
/// interrupt nobody handled could not be told from one somebody did.
///
/// No line is an answer of its own, not None: a line ends its interrupt
/// whatever its actions answer, and a number with no line has nobody to end
/// it, so the caller has to know which of the two it was.
auto Domain::handle_irq(HardwareIrq const hirqnum, Eoi const eoi) -> ustl::Option<IrqReturn> {
    auto *const line = line_by_source(hirqnum);
    if (!line) {
        return ustl::none();
    }

    return ustl::some(line->handle_irq(eoi));
}

auto Domain::is_kernel_owned(IrqId const virqnum) -> bool {
    auto *const line = line_by_id(virqnum);
    return line != nullptr && line->is_kernel_owned();
}

auto Domain::spurious_count(IrqId virqnum) -> ustl::Option<u64> {
    auto const *const line = line_by_id(virqnum);
    if (!line) {
        return ustl::none();
    }

    return ustl::some(line->spurious_count());
}

} // namespace ours::irq
