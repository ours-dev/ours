/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/gaf.hpp>
#include <ours/object/virtual_interrupt_dispatcher.hpp>

#include <heap/new.hpp>

namespace ours::object {
VirtualInterruptDispatcher::VirtualInterruptDispatcher() : Base(Flags::Virtual) {}

auto VirtualInterruptDispatcher::create(KernelHandle<InterruptDispatcher> *const out,
                                        Rights *const rights, InterruptOptions const options)
    -> Status {
    if (options != InterruptOptions::Virtual) {
        return Status::InvArgs;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self());
    if (!self) {
        return Status::NoMemory;
    }
    // Nothing is owed yet. No handle exists, so no one can be told.
    self->update_state(Signals::None, Signals::Idle);
    *out = KernelHandle<InterruptDispatcher>(ustl::move(self));
    *rights = default_right();
    return Status::Ok;
}

auto VirtualInterruptDispatcher::unmask_interrupt() -> void {
    update_state(Signals::None, Signals::Idle);
}

auto VirtualInterruptDispatcher::on_trigger() -> void {
    update_state(Signals::Idle, Signals::None);
}

auto VirtualInterruptDispatcher::on_destroy() -> void {
    update_state(Signals::None, Signals::Idle);
}
} // namespace ours::object
