/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_MP_HPP
#define OURS_ARCH_X86_MP_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/types.hpp>

/// x86's own multiprocessor calls. The ones generic code makes are declared
/// in <ours/arch_mp.hpp>; their bodies are in mp.cpp beside these.
namespace ours {
namespace task {
    class Thread;
} // namespace task

auto x86_bring_aps(CpuMask) -> void;

extern "C" [[noreturn]] CPU_INIT_CODE USED
auto x86_start_nonboot_cpu(CpuNum cpunum, task::Thread *thread) -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_MP_HPP
