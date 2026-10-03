/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ORT_MAIN_HPP
#define ORT_MAIN_HPP 1

/// The program's main, as the runtime calls it: include this in the file that
/// defines main.
///
/// Needed because user programs are compiled -ffreestanding, and in a
/// freestanding program main is an ordinary function to the compiler: its
/// name is mangled (_Z4mainiPPc), and the runtime's entry point, which calls
/// the symbol `main`, would not find it. Declared extern "C" here, the
/// definition that follows takes the same linkage. Zircon's programs have no
/// such header because they are compiled hosted, where main is never
/// mangled (its crt1.S simply names it). Hosted, this very declaration is an
/// error (-Wmain), so nothing built for the host may include it.
///
/// main's return value is the process's exit code: the runtime passes it to
/// process_exit. Unlike a hosted main there is no implicit `return 0` at the
/// end -- a main that can run off its end is a -Wreturn-type error here.
extern "C" auto main(int argc, char **argv) -> int;

#endif // #ifndef ORT_MAIN_HPP
