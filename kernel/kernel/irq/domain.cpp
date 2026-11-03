#include <ours/assert.hpp>
#include <ours/mm/gaf.hpp>

#include <algorithm>
#include <utility>

#include <heap/new.hpp>
#include <kernel/kernel/irq/domain.hpp>
#include <kernel/kernel/irq/line.hpp>
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
    if (storage.ids.resize(slots).is_err() || storage.sources.resize(sources.size()).is_err() ||
        storage.lines.resize(slots).is_err()) {
        return Status::OutOfMem;
    }
    std::ranges::fill(storage.ids, IrqId::Invalid);
    for (auto const source: sources) {
        auto const index = static_cast<usize>(source);
        if (storage.ids[index] != IrqId::Invalid) {
            return Status::AlreadyExists;
        }
        storage.ids[index] = IrqId{0};
    }
    auto next = first;
    for (usize index = 0; index < slots; ++index) {
        if (storage.ids[index] == IrqId::Invalid) {
            continue;
        }
        auto const source = static_cast<HardwareIrq>(index);
        auto const id = static_cast<IrqId>(next);
        storage.ids[index] = id;
        storage.sources[next - first] = source;
        auto line = ustl::Box<Line>::adopt(new (mm::GAF_KERNEL) Line(source, id));
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

auto Domain::irq_for_source(HardwareIrq const source) const -> IrqId {
    if (static_cast<usize>(source) >= m_storage.ids.size()) {
        return IrqId::Invalid;
    }
    return m_storage.ids[static_cast<usize>(source)];
}

auto Domain::source_for_irq(IrqId const id) const -> HardwareIrq {
    if (id < m_first ||
        static_cast<u32>(id) - static_cast<u32>(m_first) >= m_storage.sources.size()) {
        return HardwareIrq::Invalid;
    }
    return m_storage.sources[static_cast<u32>(id) - static_cast<u32>(m_first)];
}

/// The control path. A number that is no virq of the domain, IrqId::Invalid
/// included, becomes HardwareIrq::Invalid, which is past the table.
auto Domain::line_by_id(IrqId const id) -> Line * {
    return line_by_source(source_for_irq(id));
}

/// The interrupt path, and the second half of the control path.
auto Domain::line_by_source(HardwareIrq const source) -> Line * {
    if (static_cast<usize>(source) >= m_storage.lines.size()) {
        return nullptr;
    }

    return m_storage.lines[static_cast<usize>(source)].as_ptr_mut();
}

auto Domain::find_line(IrqId const id, task::IrqRcuReadGuard const &) -> ktl::Result<Line *> {
    if (id == IrqId::Invalid) {
        return ustl::err(Status::InvArgs);
    }

    auto *const line = line_by_id(id);
    if (!line) {
        return ustl::err(Status::NoResource);
    }
    return ustl::ok(line);
}

/// The registration knows its line, so the caller names only the registration: on a
/// shared line a number alone cannot say which of its registrations to remove.
auto Domain::detach(Registration &registration) -> Status {
    auto line = line_by_id(registration.line_id());
    if (!line) {
        return Status::NotFound;
    }

    return line->detach(registration);
}

auto Domain::revoke_line(IrqId const id) -> Status {
    task::IrqRcuReadGuard const guard;
    auto const line = find_line(id, guard);
    if (line.is_err()) {
        return line.unwrap_err();
    }
    if (auto const status = (*line)->mark_dying(); status != Status::Ok) {
        return status;
    }
    (*line)->revoke();
    return Status::Ok;
}

auto Domain::unmark_dying(IrqId const id) -> void {
    if (auto *const line = line_by_id(id)) {
        line->unmark_dying();
    }
}

/// Answers nothing: a line that is not there holds nothing to finalize, and
/// the line checks under its own lock whether the registration is still on it.
auto Domain::finalize_oneshot(IrqId const id, Registration const &registration) -> void {
    auto *const line = line_by_id(id);
    if (!line) {
        return;
    }
    line->finalize_oneshot(registration);
}

/// Answers what the line's registrations answered. It used to answer Handled
/// whatever happened, even on a line with no registration at all, so an
/// interrupt nobody handled could not be told from one somebody did.
///
/// No line is an answer of its own, not None: a line ends its interrupt
/// whatever its registrations answer, and a number with no line has nobody to end
/// it, so the caller has to know which of the two it was.
auto Domain::handle_irq(HardwareIrq const source, Eoi const eoi) -> ustl::Option<IrqReturn> {
    auto *const line = line_by_source(source);
    if (!line) {
        return ustl::none();
    }

    return ustl::some(line->handle_irq(eoi));
}

auto Domain::is_kernel_owned(IrqId const id) -> bool {
    auto *const line = line_by_id(id);
    return line != nullptr && line->is_kernel_owned();
}

auto Domain::spurious_count(IrqId id) -> ustl::Option<u64> {
    auto const *const line = line_by_id(id);
    if (!line) {
        return ustl::none();
    }

    return ustl::some(line->spurious_count());
}

} // namespace ours::irq
