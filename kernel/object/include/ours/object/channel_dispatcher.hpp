/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_CHANNEL_DISPATCHER_HPP
#define OURS_OBJECT_CHANNEL_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>

namespace ours::object {
/// Zircon ZX_DEFAULT_CHANNEL_RIGHTS: no Duplicate, so that a channel end has
/// exactly one holder unless someone deliberately mints a second.
constexpr auto RIGHTS_DEFAULT_CHANNEL =
    (Rights::Basic & ~Rights::Duplicate) | Rights::Io | Rights::Signal | Rights::SignalPeer;

using ChannelDispatcherBase =
    PeeredDispatcher<ChannelDispatcher, RIGHTS_DEFAULT_CHANNEL, Signals::None>;

class ChannelDispatcher: public ChannelDispatcherBase {
    using Base = ChannelDispatcherBase;
    using Self = ChannelDispatcher;

  public:
    static auto create(KernelHandle<Self> *handle0, KernelHandle<Self> *handle1, Signals *signals)
        -> Status;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_CHANNEL_DISPATCHER_HPP