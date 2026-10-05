/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_STACK_ROOT_HPP
#define ARCH_STACK_ROOT_HPP 1
// Where a thread's kernel stack begins, for an unwinder that has to tell the
// end of a stack from code it cannot step through. Same shape as
// arch/syscall.hpp: the places are fixed by each architecture's entry stubs,
// so each arch/<arch>/stack_root.hpp defines arch::StackRoot and
// arch::stack_roots() outright.
#if defined(ARCH_X86)
#include <arch/x86/stack_root.hpp>
#endif
#endif // ARCH_STACK_ROOT_HPP
