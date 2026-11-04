/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/alloc_flags.hpp>
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

    auto self = ustl::Arc<Self>::adopt(new (mm::ALLOC_KERNEL) Self());
    if (!self) {
        return Status::NoMemory;
    }
    // Nothing is owed yet. No handle exists, so no one can be told.
    self->sync_signals();
    *out = KernelHandle<InterruptDispatcher>(ustl::move(self));
    *rights = default_right();
    return Status::Ok;
}

auto VirtualInterruptDispatcher::unmask_interrupt() -> void {
    sync_signals();
}
} // namespace ours::object
