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
    *out = KernelHandle<InterruptDispatcher>(ustl::move(self));
    *rights = default_right();
    return Status::Ok;
}
} // namespace ours::object
