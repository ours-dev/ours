/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/pager_dispatcher.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/vmo.hpp>

namespace ours::syscall {
using object::KernelHandle;
using object::PagerDispatcher;
using object::PortDispatcher;
using object::ProcessDispatcher;
using object::VmoDispatcher;

namespace {
    /// The handle, then the value out to the caller, and only then the handle
    /// into the table, as vmo_create publishes (design 4.1): a failure at any
    /// step leaves nothing the program could have seen.
    auto publish(KernelHandle<object::Dispatcher> dispatcher, Rights const rights,
                 user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
        // Counts against this process's handle quota: make() is what refuses
        // past it.
        auto handle = object::Handle::make(ustl::move(dispatcher), rights);
        if (!handle) {
            return Status::NoResource;
        }
        auto &table = ProcessDispatcher::current()->handle_table();
        if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
            return status;
        }
        auto _ = table.add(ustl::move(handle));
        return Status::Ok;
    }
} // namespace

/// A new pager, and a handle to it. The pager checks |options|.
auto pager_create(u32 const options, user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    KernelHandle<PagerDispatcher> pager;
    Rights rights = Rights::None;
    if (auto const status = PagerDispatcher::create(options, pager, rights); status != Status::Ok) {
        return status;
    }
    return publish(KernelHandle<object::Dispatcher>(ustl::move(pager)), rights, out);
}

/// Both handles first, in argument order, so that a bad handle outranks every
/// argument error (design 4.5). The pager needs AttachVmo, to tie a VMO to
/// itself; the port Write, since the VMO's requests are queued on it.
/// Everything about the options and the size is the pager's.
auto pager_create_vmo(ours::Uandle const pager, VmoOptions const options, ours::Uandle const port,
                      u64 const key, u64 const size, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<PagerDispatcher> pager_dispatcher;
    if (auto const status = table.get_dispatcher(pager, Rights::AttachVmo, &pager_dispatcher);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<PortDispatcher> port_dispatcher;
    if (auto const status = table.get_dispatcher(port, Rights::Write, &port_dispatcher);
        status != Status::Ok) {
        return status;
    }

    KernelHandle<VmoDispatcher> vmo;
    Rights rights = Rights::None;
    if (auto const status = pager_dispatcher->create_vmo(options, ustl::move(port_dispatcher), key,
                                                         size, vmo, rights);
        status != Status::Ok) {
        return status;
    }
    return publish(KernelHandle<object::Dispatcher>(ustl::move(vmo)), rights, out);
}

/// The three handles first, in argument order (design 4.5). The pager needs
/// ManageVmo; |pager_vmo| Write, since its contents change; |aux_vmo| Read
/// and Write, since its pages are read out and it reads zero after. Whether
/// |pager_vmo| is the pager's, and everything about the ranges, is the
/// pager's.
auto pager_supply_pages(ours::Uandle const pager, ours::Uandle const pager_vmo, u64 const offset,
                        u64 const length, ours::Uandle const aux_vmo, u64 const aux_offset)
    -> Status {
    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<PagerDispatcher> pager_dispatcher;
    if (auto const status = table.get_dispatcher(pager, Rights::ManageVmo, &pager_dispatcher);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<VmoDispatcher> vmo;
    if (auto const status = table.get_dispatcher(pager_vmo, Rights::Write, &vmo);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<VmoDispatcher> aux;
    if (auto const status = table.get_dispatcher(aux_vmo, Rights::Read | Rights::Write, &aux);
        status != Status::Ok) {
        return status;
    }
    return pager_dispatcher->supply_pages(*vmo, offset, length, *aux, aux_offset);
}

/// Both handles first (design 4.5). The pager needs AttachVmo and ManageVmo,
/// since detaching undoes the one and ends the other; the VMO Write, since
/// its unsupplied pages stop reading.
auto pager_detach_vmo(ours::Uandle const pager, ours::Uandle const vmo) -> Status {
    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<PagerDispatcher> pager_dispatcher;
    if (auto const status =
            table.get_dispatcher(pager, Rights::AttachVmo | Rights::ManageVmo, &pager_dispatcher);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<VmoDispatcher> vmo_dispatcher;
    if (auto const status = table.get_dispatcher(vmo, Rights::Write, &vmo_dispatcher);
        status != Status::Ok) {
        return status;
    }
    return pager_dispatcher->detach_vmo(*vmo_dispatcher);
}
} // namespace ours::syscall
