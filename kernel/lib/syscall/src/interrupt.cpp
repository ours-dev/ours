/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/virtual_interrupt_dispatcher.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
using object::InterruptDispatcher;
using object::KernelHandle;
using object::ProcessDispatcher;
using object::VirtualInterruptDispatcher;

namespace {
    /// The options interrupt_create knows. Anything else is refused before
    /// the kind is looked at, as Zircon's Create() functions refuse options
    /// they do not permit.
    constexpr auto KNOWN_INTERRUPT_OPTIONS = InterruptOptions::Virtual;
} // namespace

/// Zircon: sys_interrupt_create() (kernel/lib/syscalls/driver.cc). Zircon
/// validates the IRQ resource first for every interrupt that is not virtual;
/// a virtual one needs none, so |resource| and |line| are not looked at.
/// Physical lines are not here yet: a create without Virtual is
/// NotSupported rather than a stand-in.
auto interrupt_create([[maybe_unused]] ours::Uandle const resource, [[maybe_unused]] u32 const line,
                      InterruptOptions const options, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    if (!!(options & ~KNOWN_INTERRUPT_OPTIONS)) {
        return Status::InvArgs;
    }
    if (!(options & InterruptOptions::Virtual)) {
        return Status::NotSupported;
    }

    KernelHandle<InterruptDispatcher> interrupt;
    Rights rights = Rights::None;
    if (auto const status = VirtualInterruptDispatcher::create(&interrupt, &rights, options);
        status != Status::Ok) {
        return status;
    }
    auto handle =
        object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(interrupt)), rights);
    if (!handle) {
        return Status::NoResource;
    }

    // The value goes out before the handle goes in, as port_create does: a
    // fault here leaves no handle behind, and the object goes with it.
    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// Zircon: sys_interrupt_wait(). The interrupt is taken before the time is
/// written out, so a |out_timestamp| that faults loses it, as in Zircon.
auto interrupt_wait(ours::Uandle const handle, user_copy::UserOutPtr<Instant> const out_timestamp)
    -> Status {
    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Wait, &interrupt);
        status != Status::Ok) {
        return status;
    }

    Instant timestamp = 0;
    if (auto const status = interrupt->wait(&timestamp); status != Status::Ok) {
        return status;
    }
    if (out_timestamp.is_null()) {
        return Status::Ok;
    }
    return out_timestamp.copy_to_user(timestamp);
}

/// Zircon: sys_interrupt_trigger(). The options before the handle, as there.
auto interrupt_trigger(ours::Uandle const handle, InterruptTriggerOptions const options,
                       Instant const timestamp) -> Status {
    if (options != InterruptTriggerOptions::None) {
        return Status::InvArgs;
    }

    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Signal, &interrupt);
        status != Status::Ok) {
        return status;
    }
    return interrupt->trigger(timestamp);
}

/// Zircon: sys_interrupt_destroy(), which asks for no right: any holder of a
/// handle may end the interrupt, as closing the last handle would.
auto interrupt_destroy(ours::Uandle const handle) -> Status {
    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &interrupt);
        status != Status::Ok) {
        return status;
    }
    return interrupt->destroy();
}
} // namespace ours::syscall
