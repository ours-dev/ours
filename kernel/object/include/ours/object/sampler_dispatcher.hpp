/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_SAMPLER_DISPATCHER_HPP
#define OURS_OBJECT_SAMPLER_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/sampler.hpp>
#include <ours/types.hpp>

#include <thread_sampler/thread_sampler.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/box.hpp>

namespace ours::object {
/// Rights a freshly created sampler handle carries: it can be passed on,
/// duplicated and inspected. No call on it asks for a right: holding the
/// handle is the capability, and the SAMPLING resource was what it took to
/// get one.
constexpr auto RIGHTS_DEFAULT_SAMPLER = Rights::Transfer | Rights::Duplicate | Rights::Inspect;

class SamplerDispatcher;
using SamplerDispatcherBase = SoloDispatcher<SamplerDispatcher, RIGHTS_DEFAULT_SAMPLER>;

/// A sampling profiler session as a kernel object: its handle stands for
/// "this caller owns the system's profiling session".
///
/// The session (sampler::Session) does the work and keeps the state; this
/// object owns it, so that it lives as long as anything refers to the
/// object. Closing the last handle releases the session -- stopped if it
/// runs, and the system's one slot freed for the next create -- while the
/// buffers stay until the last reference goes, which a sampler_read still
/// copying on another thread holds.
class SamplerDispatcher final: public SamplerDispatcherBase {
    using Base = SamplerDispatcherBase;
    using Self = SamplerDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Sampler);

    /// A new session for |config|, which the caller has read from user
    /// memory but not checked.
    ///
    /// InvArgs for a reserved field that is not zero, targets that name
    /// nothing or a bit this kernel does not know, or what
    /// sampler::Session::create() refuses (the period and buffer size);
    /// AlreadyExists while another session exists; NoMemory.
    [[nodiscard]]
    static auto create(SamplerConfig const &config, KernelHandle<SamplerDispatcher> *out,
                       Rights *rights) -> Status;

    [[nodiscard]]
    auto start() -> Status;

    [[nodiscard]]
    auto stop() -> Status;

    /// Copies the dump to |data|, which has room for |size| bytes, and
    /// writes how many went to |actual|; with |data| null, writes the size
    /// the dump needs instead. See sampler::Session::read() for the answers.
    [[nodiscard]]
    auto read(user_copy::UserOutPtr<u8> data, usize size, usize *actual) -> Status;

    /// The last handle is gone: the session is released here rather than
    /// when the last reference drops, so that a create can succeed at once.
    auto on_zero_handles() -> void final;

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the canary.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("SamplerDispatcher");

    explicit SamplerDispatcher(ustl::Box<sampler::Session> session);

    /// Never null: create() makes the session before the object.
    ustl::Box<sampler::Session> m_session;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_SAMPLER_DISPATCHER_HPP
