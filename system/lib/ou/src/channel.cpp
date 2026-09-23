/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/channel.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto channel_create(ours::HandleValue *const out0, ours::HandleValue *const out1,
                    ours::ChannelCreateOptions const options) -> ours::Status {
    return ours_channel_create(options, out0, out1);
}

auto channel_write(ours::HandleValue const channel, void const *const bytes, u32 const num_bytes)
    -> ours::Status {
    return ours_channel_write(channel, ours::ChannelWriteOptions::None, bytes, num_bytes, nullptr,
                              0);
}

auto channel_read(ours::HandleValue const channel, void *const bytes, u32 const num_bytes,
                  u32 *const actual_bytes) -> ours::Status {
    return ours_channel_read(channel, ours::ChannelReadOptions::None, bytes, nullptr, num_bytes, 0,
                             actual_bytes, nullptr);
}
} // namespace ou
