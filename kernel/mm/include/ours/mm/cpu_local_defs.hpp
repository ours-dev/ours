/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_MM_CPU_LOCAL_DEFS_HPP
#define OURS_MM_CPU_LOCAL_DEFS_HPP 1

#include <ours/compiler.hpp>

#define CPU_LOCAL LINK_SECTION(".init.data.cpu_local") USED
#define CPU_LOCAL_SPECIAL(GROUP) LINK_SECTION(".init.data.cpu_local" GROUP) USED

#endif // #ifndef OURS_MM_CPU_LOCAL_DEFS_HPP