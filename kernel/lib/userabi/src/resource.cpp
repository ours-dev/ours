/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "resource.hpp"

#include <logz4/log.hpp>
#include <ours/object/resource_dispatcher.hpp>

namespace userabi {
using namespace ours;

namespace {
    auto make_resource_handle(RsrcKind const kind, u64 const base, u64 const size,
                              char const *const name) -> ustl::Box<object::Handle> {
        object::KernelHandle<object::ResourceDispatcher> resource;
        Rights rights = Rights::None;
        auto const status = object::ResourceDispatcher::create(
            &resource, rights, kind, PhysAddr(base), PhysAddr(size), RsrcFlag{},
            object::ResourceDispatcher::Name(name));
        if (Status::Ok != status) {
            log::error("userabi: could not mint the {} resource: {}", name, to_string(status));
            return {};
        }

        auto handle = object::Handle::make(
            object::KernelHandle<object::Dispatcher>(ustl::move(resource)), rights);
        if (!handle) {
            log::error("userabi: could not make a handle to the {} resource", name);
        }
        return handle;
    }
} // namespace

auto make_system_resource_handle(u64 const base, char const *const name)
    -> ustl::Box<object::Handle> {
    return make_resource_handle(RsrcKind::System, base, 1, name);
}

auto make_root_resource_handle(RsrcKind const kind, char const *const name)
    -> ustl::Box<object::Handle> {
    return make_resource_handle(kind, 0, 0, name);
}
} // namespace userabi
