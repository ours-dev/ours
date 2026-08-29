/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_IDLE_THREAD_HPP
#define OURS_ARCH_IDLE_THREAD_HPP 1

namespace ours {
class ArchIdleThread {
    using Self = ArchIdleThread;

  public:
    auto idle() -> void;
};

} // namespace ours

#endif // #ifndef OURS_ARCH_IDLE-THREAD_HPP