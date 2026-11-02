/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CHANNEL_HPP
#define OURS_CHANNEL_HPP 1

// The channel calls, their limits and their option words, one domain in one
// file: a program that speaks channels includes this and nothing else of the
// system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
/// The most bytes one channel message may carry.
///
/// 4 KiB, which is the largest single kmalloc: a message is copied into one
/// kernel allocation before the channel's lock is taken, and that allocation
/// does not go through vmalloc, whose 256 regions are shared by the whole
/// system. Raising this later is compatible, lowering it would not be.
inline constexpr auto CHANNEL_MAX_MSG_BYTES = u32{4096};

/// The most handles one channel message may carry.
inline constexpr auto CHANNEL_MAX_MSG_HANDLES = u32{64};

/// Options for the channel calls. None is the only value any of them accepts
/// today; anything else answers InvArgs rather than being ignored, so that a
/// program cannot come to depend on a bit that does nothing. They are
/// separate types, one per call, because the bits each will grow are
/// unrelated (a write taking its bytes from a scatter list and a read allowed
/// to discard what does not fit share no meaning), and a type per call makes
/// passing one to the other a compile error.
enum class ChannelCreateOptions : u32 {
    None = 0,
};

enum class ChannelWriteOptions : u32 {
    None = 0,
};

enum class ChannelReadOptions : u32 {
    None = 0,
};

// The channel calls, as a program calls them: generated from
// //system/public/ours/syscalls/channel.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/channel.inc>
} // namespace ours

#endif // OURS_CHANNEL_HPP
