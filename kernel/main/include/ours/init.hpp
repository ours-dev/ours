/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_INIT_HPP
#define OURS_INIT_HPP 1

#include <ours/compiler.hpp>

#define INIT_SECTION(name) LINK_SECTION(".init." name)

/// The code marked this just called once on boot time.
#define INIT_CODE      INIT_SECTION("text")

/// The data marked this just used on boot time.
#define INIT_DATA      INIT_SECTION("data")

/// The read only data marked this just read on boot time.
#define INIT_CONST     INIT_SECTION("rodata")

#ifdef CONFIG_MEM_HOTPLUG
#   define MEM_INIT_CODE
#   define MEM_INIT_DATA
#   define MEM_INIT_CONST
#else
#   define MEM_INIT_CODE    INIT_CODE
#   define MEM_INIT_DATA    INIT_DATA
#   define MEM_INIT_CONST   INIT_CONST
#endif // #ifdef CONFIG_MEM_HOTPLUG

#endif // #ifndef OURS_INIT_HPP