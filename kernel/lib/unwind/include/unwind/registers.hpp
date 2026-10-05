/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef UNWIND_REGISTERS_HPP
#define UNWIND_REGISTERS_HPP 1

#include <cstddef>
#include <cstdint>

namespace unwind {

/// The register subset a CFA-based walk needs. `pc` is the instruction
/// pointer of the frame being unwound *from*, `sp`/`bp` the corresponding
/// stack pointer and frame pointer as they were at `pc`.
///
/// x86-64 DWARF register numbers this library relies on: `RBP` is 6, `RSP`
/// is 7 and the return-address column is conventionally 16.
struct Registers final {
    std::uint64_t pc;
    std::uint64_t sp;
    std::uint64_t bp;

    /// Whether `pc` is a return address, read off the stack by a step,
    /// rather than the exact pc of an interrupted instruction. A return
    /// address points just past its call; when the call is the last
    /// instruction of a function, that is past the function, into the next
    /// one or into no function at all. The call, and the rules that hold at
    /// it, are at `pc - 1`, which is still inside the caller. Unwinder::step()
    /// sets it; whoever builds the first frame leaves it false.
    bool pc_is_return_address = false;

    static constexpr std::uint64_t RBP = 6;
    static constexpr std::uint64_t RSP = 7;
    static constexpr std::uint64_t RIP = 16;
};

} // namespace unwind

#endif // #ifndef UNWIND_REGISTERS_HPP
