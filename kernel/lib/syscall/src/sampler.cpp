/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/object/sampler_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall/resource.hpp>

namespace ours::syscall {
using object::KernelHandle;
using object::ProcessDispatcher;
using object::SamplerDispatcher;

/// A new sampling session. The options are checked first, then the
/// resource, then the configuration, so that a caller without the SAMPLING
/// resource learns nothing about what it would have been allowed.
auto sampler_create(ours::Uandle const resource, u64 const options,
                    user_copy::UserInPtr<SamplerConfig const> const config,
                    user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    if (options != 0) {
        return Status::InvArgs;
    }
    if (auto const status = object::validate_ranged_resource(resource, RsrcKind::System,
                                                             RSRC_SYSTEM_SAMPLING_BASE, 1);
        status != Status::Ok) {
        return status;
    }

    SamplerConfig copied{};
    if (auto const status = config.copy_from_user(&copied); status != Status::Ok) {
        return status;
    }

    KernelHandle<SamplerDispatcher> sampler;
    Rights rights = Rights::None;
    if (auto const status = SamplerDispatcher::create(copied, &sampler, &rights);
        status != Status::Ok) {
        return status;
    }
    auto handle =
        object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(sampler)), rights);
    if (!handle) {
        return Status::NoResource;
    }

    // The value goes out before the handle goes in, as port_create does: a
    // fault here leaves no handle behind, and the session goes with the
    // object, released by its destructor.
    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

auto sampler_start(ours::Uandle const handle) -> Status {
    ustl::Arc<SamplerDispatcher> sampler;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &sampler);
        status != Status::Ok) {
        return status;
    }
    return sampler->start();
}

auto sampler_stop(ours::Uandle const handle) -> Status {
    ustl::Arc<SamplerDispatcher> sampler;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &sampler);
        status != Status::Ok) {
        return status;
    }
    return sampler->stop();
}

/// The reference |sampler| holds is what keeps the session's buffers alive
/// for the copy, should another thread close the last handle meanwhile.
/// |actual| is written whatever the answer, when the read got that far: on
/// BufferTooSmall it is the size needed, and after a fault the bytes copied
/// before it.
auto sampler_read(ours::Uandle const handle, user_copy::UserOutPtr<u8> const data,
                  usize const data_size, user_copy::UserOutPtr<usize> const actual) -> Status {
    ustl::Arc<SamplerDispatcher> sampler;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &sampler);
        status != Status::Ok) {
        return status;
    }

    usize count = 0;
    auto const status = sampler->read(data, data_size, &count);
    if (status != Status::Ok && status != Status::BufferTooSmall && count == 0) {
        return status;
    }
    if (auto const copied = actual.copy_to_user(count); copied != Status::Ok) {
        return copied;
    }
    return status;
}
} // namespace ours::syscall
