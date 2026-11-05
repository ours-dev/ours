/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/alloc_flags.hpp>
#include <ours/object/sampler_dispatcher.hpp>

#include <heap/new.hpp>

namespace ours::object {
namespace {
    /// The targets this kernel knows.
    constexpr auto KNOWN_TARGETS = SamplerTargets::User | SamplerTargets::Kernel;
} // namespace

SamplerDispatcher::SamplerDispatcher(ustl::Box<sampler::Session> session)
    : m_session(ustl::move(session)) {}

auto SamplerDispatcher::create(SamplerConfig const &config, KernelHandle<SamplerDispatcher> *out,
                               Rights *rights) -> Status {
    if (config.reserved != 0) {
        return Status::InvArgs;
    }
    if (config.targets == SamplerTargets::None || !!(config.targets & ~KNOWN_TARGETS)) {
        return Status::InvArgs;
    }
    // Checked here, against the ABI's limits, before the narrowing below:
    // the buffer size is a u64 in the ABI and a usize in the session, which
    // differ on a 32-bit target, where a size past usize would wrap into one
    // the session accepts.
    if (config.buffer_size > SAMPLER_MAX_BUFFER_SIZE || config.period < SAMPLER_MIN_PERIOD ||
        config.period > SAMPLER_MAX_PERIOD) {
        return Status::InvArgs;
    }

    ustl::Box<sampler::Session> session;
    auto const session_config = sampler::Config{
        .period = Duration(config.period),
        .buffer_size = static_cast<usize>(config.buffer_size),
        .targets = config.targets,
    };
    if (auto const status = sampler::Session::create(session_config, &session);
        status != Status::Ok) {
        return status;
    }

    // If the object cannot be made, the session's destructor releases it, so
    // a failed create leaves no session claimed.
    auto self = ustl::Arc<Self>::adopt(new (mm::ALLOC_KERNEL) Self(ustl::move(session)));
    if (!self) {
        return Status::NoMemory;
    }
    *rights = default_right();
    *out = KernelHandle<SamplerDispatcher>(ustl::move(self));
    return Status::Ok;
}

auto SamplerDispatcher::start() -> Status {
    return m_session->start();
}

auto SamplerDispatcher::stop() -> Status {
    return m_session->stop();
}

auto SamplerDispatcher::read(user_copy::UserOutPtr<u8> const data, usize const size,
                             usize *const actual) -> Status {
    if (data.is_null()) {
        return m_session->dump_size(actual);
    }
    // Each piece goes to |data| at its offset in the dump. A copy that faults
    // ends the read with the fault's status; what was copied before it
    // stays, and |actual| says how much that was.
    auto base = data;
    auto const to_user = [](void *context, usize offset, void const *bytes, usize len) -> Status {
        auto const *start = static_cast<user_copy::UserOutPtr<u8> const *>(context);
        return start->element_offset(offset).copy_array_to_user(static_cast<u8 const *>(bytes),
                                                                len);
    };
    return m_session->read({&base, to_user}, size, actual);
}

auto SamplerDispatcher::on_zero_handles() -> void {
    m_session->release();
}
} // namespace ours::object
