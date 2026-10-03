/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ORT_STARTUP_HANDLES_HPP
#define ORT_STARTUP_HANDLES_HPP 1

#include <span>

#include <ours/process_args.hpp>
#include <ours/types.hpp>

namespace ort {
/// The handles a process was started with, each tagged with its handle_info,
/// waiting to be claimed.
///
/// Claiming is taking: take() hands the handle over and forgets it, so each
/// is owned by exactly one piece of code, and a second take of the same
/// thing finds nothing -- the way Zircon's zx_take_startup_handle() clears
/// its slot. A handle nobody takes stays open for the life of the process,
/// as in Zircon; closing unclaimed handles at some point would need that
/// point to come after every taker, which only the program knows.
///
/// Holds no storage of its own: two parallel arrays the caller provides and
/// keeps alive, one of handle values and one of their handle_infos. No lock
/// either: a process has one thread for now (the P0 single-thread rule,
/// docs/design/260923/userspace-interface.md 4.8), and a second thread
/// calling take() alongside the first would need one.
class StartupHandles {
  public:
    StartupHandles() = default;

    /// |handles[i]| is described by |infos[i]|; the two must be the same
    /// length.
    StartupHandles(std::span<ours::Uandle> handles, std::span<u32> infos);

    /// Takes the first handle whose handle_info is exactly (|type|, |arg|):
    /// both have to match. Uandle::Invalid when there is none, or it was
    /// taken already.
    [[nodiscard]]
    auto take(ours::HandleType type, u16 arg) -> ours::Uandle;

  private:
    std::span<ours::Uandle> m_handles;
    std::span<u32> m_infos;
};
} // namespace ort

#endif // #ifndef ORT_STARTUP_HANDLES_HPP
