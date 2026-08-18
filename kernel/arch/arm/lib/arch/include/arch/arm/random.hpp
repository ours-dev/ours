/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_ARM_RANDOM_HPP
#define ARCH_ARM_RANDOM_HPP 1

// Only ever reached via <arch/random.hpp>'s own #include — don't include
// this file directly.

namespace arch {

// RNDR (FEAT_RNG, ARMv8.5-A) is optional, so check ID_AA64ISAR0_EL1[63:60]
// before using it — a value of 0b0001 means the instruction is implemented.
// Written as the raw Sop0_op1_CRn_CRm_op2 register encoding (S3_3_C2_C4_0)
// rather than the `rndr` mnemonic so this doesn't need -march=+rng: reading
// an unimplemented system register this way still assembles, it's the
// feature check above that decides whether the read is meaningful.
inline auto rand(usize *value) -> bool {
    u64 isar0;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    if (((isar0 >> 60) & 0xf) != 1) {
        return false;
    }

    u64 result;
    u64 nzcv;
    asm volatile("mrs %0, s3_3_c2_c4_0\n\t"
                 "mrs %1, nzcv"
                 : "=r"(result), "=r"(nzcv));
    // RNDR clears PSTATE.Z on success, sets it (bit 30 of the packed NZCV
    // value) and returns zero if no random number was available.
    if ((nzcv & (u64(1) << 30)) != 0) {
        return false;
    }

    *value = result;
    return true;
}

} // namespace arch

#endif // ARCH_ARM_RANDOM_HPP
