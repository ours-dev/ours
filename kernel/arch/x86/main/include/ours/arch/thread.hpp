/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_THREAD_HPP
#define OURS_ARCH_THREAD_HPP 1

#include <ours/types.hpp>
#include <ours/compiler.hpp>

#include <ours/task/types.hpp>

#include <gktl/canary.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours::task {
    class X86Thread {
        using Self = X86Thread;
      public:
        static auto switch_thread(Self *prev, Self *next) -> void;

        auto init(VirtAddr entry_point) -> void;
      private:
        friend class Thread;

        VirtAddr m_fs_base;
        VirtAddr m_gs_base;
        VirtAddr m_sp;
    };
    using ArchThread = X86Thread;

} // namespace ours::task

#endif // #ifndef OURS_ARCH_THREAD_HPP