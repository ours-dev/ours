/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_CLIENT_HPP
#define LAUNCH_CLIENT_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

namespace launch {
/// A connection to the launcher service (<launch/protocol.hpp>): how a
/// program that holds no VMEX starts another. Ours' counterpart of
/// fdio_spawn() over fuchsia.process.Launcher.
///
/// Holds the channel, and does not own it: the caller closes it. One request
/// at a time, from one thread.
class Client {
  public:
    explicit Client(ours::Uandle const channel) : m_channel(channel) {}

    /// Starts the BootFS file |name| under |job| and returns its process.
    ///
    /// The program gets |args|, |environ| and |handles| in its processargs,
    /// each handle described by the entry of |handle_info| at the same index,
    /// and its root region (VmaRoot), which the launcher adds. |handles| are
    /// consumed whatever the answer, as channel_write consumes them; |job| is
    /// not: the request carries a duplicate.
    ///
    /// The answer is the launcher's -- NotFound for a name its BootFS lacks,
    /// InvArgs for a request it cannot read, or what starting the program
    /// answered -- or this side's: encode_launch()'s refusals, the duplicate
    /// of |job|, the write, PeerClosed if the launcher went away, InvArgs for
    /// a reply that is not one.
    [[nodiscard]]
    auto launch(ours::Uandle job, std::string_view name, std::span<std::string_view const> args,
                std::span<std::string_view const> environ, std::span<ours::Uandle const> handles,
                std::span<u32 const> handle_info) const -> ustl::Result<ours::Uandle, ours::Status>;

    /// A new connection to the same launcher, for another program: a channel
    /// whose other end is sent in a Connect request. The caller owns the end
    /// it gets back, and passes it on as the program's
    /// protocol::CONNECTION startup handle.
    [[nodiscard]]
    auto connect() const -> ustl::Result<ours::Uandle, ours::Status>;

    [[nodiscard]]
    auto channel() const -> ours::Uandle {
        return m_channel;
    }

  private:
    ours::Uandle m_channel;
};
} // namespace launch

#endif // #ifndef LAUNCH_CLIENT_HPP
