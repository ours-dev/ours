/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_INIT_HPP
#define OURS_INIT_HPP 1

#include <ours/compiler.hpp>

#define INIT_SECTION(name) LINK_SECTION(".init." name)

/// The code marked this just called once on boot time.
#define INIT_CODE INIT_SECTION("text")

/// The data marked this just used on boot time.
#define INIT_DATA INIT_SECTION("data")

/// Boot-time data that is only read. The section, and only the section --
/// `const` belongs at the declaration.
///
/// Named for what it does, like INIT_CODE and INIT_DATA above. It was
/// INIT_CONST, which claimed a constness it never conferred: its one user was
/// a plainly mutable array, and the name was the only thing suggesting
/// otherwise.
///
/// Teaching it to emit `const` was the other way out, and it does not work. A
/// macro can only prepend, and a leading `const` binds to the pointee: given a
/// pointer declaration it produces `const int *p` -- a mutable pointer to
/// constant data -- where the author wanted `int *const p`. It would be
/// silently wrong for exactly the declarations where constness matters and
/// right only where writing `const` by hand costs nothing. So the keyword stays
/// at the declaration, where whoever writes it can put it on the correct side.
#define INIT_RODATA INIT_SECTION("rodata")

#ifdef CONFIG_MEM_HOTPLUG
#define MEM_INIT_CODE
#define MEM_INIT_DATA
#define MEM_INIT_RODATA
#else
#define MEM_INIT_CODE INIT_CODE
#define MEM_INIT_DATA INIT_DATA
#define MEM_INIT_RODATA INIT_RODATA
#endif // #ifdef CONFIG_MEM_HOTPLUG

/// The code and data a CPU runs through to start: init sections without CPU
/// hotplug, freed after boot; kept with it, for a CPU that comes back to
/// start through them again (the build argument cpu_hotplug).
#ifdef CONFIG_CPU_HOTPLUG
#define CPU_INIT_CODE
#define CPU_INIT_DATA
#else
#define CPU_INIT_CODE INIT_CODE
#define CPU_INIT_DATA INIT_DATA
#endif // #ifdef CONFIG_CPU_HOTPLUG

#endif // #ifndef OURS_INIT_HPP