/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_NOSPEC_HPP
#define ARCH_NOSPEC_HPP 1

#include <arch/types.hpp>

// Primitives that hold under speculative execution (Spectre V1, bounds check
// bypass).
//
// An ordinary bounds check is a conditional branch, and a branch is
// predicted: while the comparison is still resolving, the CPU runs ahead down
// the predicted side with the unchecked value, and whatever that path loads
// leaves a trace in the cache that outlives the misprediction. With an index
// the user chose, that is a read of any kernel byte, one cache line at a time.
//
// These close the gap with a data dependency instead of a branch: the result
// is produced by a conditional move, which cannot execute until the
// comparison it depends on has, so no speculative path ever sees the
// unchecked value. They do not replace the ordinary check -- that is still
// what returns an error on the architectural path -- they back it up on the
// speculative one.
//
// The arch completion defines:
//
//   auto confine_array_index(usize index, usize size) -> usize;
//       |index| if it is below |size|, otherwise 0.
//
//   auto conditional_select_nospec_eq(usize x, usize y, usize a, usize b) -> usize;
//       |a| if |x| equals |y|, otherwise |b|.
//
// A plain `x == y ? a : b` is not a substitute: clang at -O2 is free to, and
// does, compile it to a branch.
//
// Zircon counterparts: fbl::confine_array_index() and
// fbl::conditional_select_nospec_eq(), in fbl/confine_array_index.h and
// fbl/conditional_select_nospec.h, which these follow instruction for
// instruction.
#if defined(ARCH_X86)
#include <arch/x86/nospec.hpp>
#else
// Written only where it can be built and checked. The other architectures
// need their own conditional select plus a speculation barrier (CSDB on
// arm64); guessing at them here, unbuilt, would be worse than failing loudly.
#error "no Spectre V1 primitives for this architecture yet"
#endif

#endif // ARCH_NOSPEC_HPP
