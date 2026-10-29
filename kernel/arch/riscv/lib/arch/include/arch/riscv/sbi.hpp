/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_SBI_HPP
#define ARCH_RISCV_SBI_HPP 1

#include <ours/prelude.hpp>

/// The Supervisor Binary Interface: how S-mode code asks the M-mode firmware
/// (OpenSBI on QEMU's `virt` machine) for what only M-mode can do -- console
/// output, timers, starting another hart, power off.
///
/// Code here relies on what the specification promises, never on what one
/// implementation happens to do: any firmware that implements SBI has to
/// boot the same image.
///
/// A call is an `ecall` from S-mode. The firmware takes the trap, looks at
/// a7 (which extension) and a6 (which function in it), reads the arguments
/// from a0..a5, and returns an error code in a0 and a value in a1. Nothing
/// else is clobbered.
namespace arch::sbi {

/// An extension ID, passed in a7.
enum class Extension : usize {
    /// Legacy `console_putchar`: one byte in a0, a status back in a0. The
    /// specification deprecates it and lets an implementation leave it out,
    /// so it is the one call here a conforming firmware may lack. It is used
    /// because, unlike the debug console, it needs no probe first.
    LegacyConsolePutchar = 0x01,
};

/// What an `ecall` hands back. `error` is zero on success and a negative SBI
/// error code otherwise; `value` is only meaningful when `error` is zero.
struct Result {
    isize error;
    usize value;
};

/// Call `function` of `extension` with up to three arguments.
///
/// Unused arguments are passed as zero. The `memory` clobber keeps the
/// compiler from moving a load or store of a buffer the firmware reads or
/// writes across the call.
[[nodiscard]]
inline auto call(Extension const extension, usize const function, usize const arg0,
                 usize const arg1, usize const arg2) -> Result {
    register usize a0 asm("a0") = arg0;
    register usize a1 asm("a1") = arg1;
    register usize a2 asm("a2") = arg2;
    register usize a6 asm("a6") = function;
    register usize a7 asm("a7") = static_cast<usize>(extension);
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a6), "r"(a7) : "memory");
    return {.error = static_cast<isize>(a0), .value = a1};
}

/// Write one byte to the firmware's console.
inline auto console_putchar(char const ch) -> void {
    // A legacy call returns its status in a0, which this has no way to act
    // on: a console that cannot print cannot report that either.
    auto _ = call(Extension::LegacyConsolePutchar, 0, static_cast<usize>(ch), 0, 0);
}

} // namespace arch::sbi

#endif // ARCH_RISCV_SBI_HPP
