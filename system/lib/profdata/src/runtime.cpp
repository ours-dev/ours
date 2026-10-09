/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// PROVENANCE: adapted from Fuchsia's src/lib/llvm-profdata (Copyright 2021 The
/// Fuchsia Authors, BSD-style license).

#include <profile/InstrProfData.inc>

// NOLINTBEGIN(readability-identifier-naming): a symbol the compiler's
// instrumentation refers to by name, so it cannot follow this repository's case
// rules.
extern "C" {

// The compiler emits a reference to this from every instrumented module, to make
// sure a runtime is linked in; this definition is what answers it, in a program
// that has no compiler runtime to answer it.
//
// It is a target of its own, and not part of RawProfile's, because a program
// that does link the compiler's runtime must not link this: the symbol would
// answer for the runtime, the linker would then have no reason to pull in the
// part of it that writes the profile when the program exits, and that program
// would write none. Whoever links this with the compiler's runtime gets a
// duplicate-symbol error, which is the right way to find out.
extern int const INSTR_PROF_PROFILE_RUNTIME_VAR = 0;

} // extern "C"
// NOLINTEND(readability-identifier-naming)
