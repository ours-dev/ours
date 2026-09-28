/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "resource.hpp"

#include <logz4/log.hpp>
#include <ours/object/resource_dispatcher.hpp>

namespace userabi {
using namespace ours;

auto make_system_resource_handle(u64 const base, char const *const name)
    -> ustl::Box<object::Handle> {
    object::KernelHandle<object::ResourceDispatcher> resource;
    Rights rights = Rights::None;
    auto const status =
        object::ResourceDispatcher::create(&resource, rights, RsrcKind::System, PhysAddr(base), 1,
                                           RsrcFlag{}, object::ResourceDispatcher::Name(name));
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
} // namespace userabi
