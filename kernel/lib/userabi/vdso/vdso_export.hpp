/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VDSO_VDSO_EXPORT_HPP
#define OURS_VDSO_VDSO_EXPORT_HPP 1

/// One published word in the vDSO's read-only data.
///
/// The linker gathers every use into `.vdso_data`. That section is allocated
/// and not writable, so the vDSO still has no writable segment. The kernel
/// writes the bytes through the vDSO VMO. |initial| is the value in the image
/// before that write, a constant the assembler can emit as `.quad`.
#define VDSO_EXPORT(type, name, initial)                                                           \
    asm(".section .vdso_data,\"a\",@progbits\n\t"                                                  \
        ".balign 8\n\t"                                                                            \
        ".globl " #name "\n\t"                                                                     \
        ".hidden " #name "\n\t"                                                                    \
        ".type " #name ", @object\n\t" #name ":\n\t"                                               \
        ".quad " #initial "\n\t"                                                                   \
        ".size " #name ", 8\n\t"                                                                   \
        ".previous\n\t");                                                                          \
    extern "C" type name

/// A published NUL-terminated string in the same section. |literal| is a
/// macro that expands to a string literal.
#define VDSO_EXPORT_STRING(name, literal) VDSO_EXPORT_STRING_QUOTED(name, literal)
#define VDSO_EXPORT_STRING_QUOTED(name, literal)                                                   \
    asm(".section .vdso_data,\"a\",@progbits\n\t"                                                  \
        ".balign 8\n\t"                                                                            \
        ".globl " #name "\n\t"                                                                     \
        ".hidden " #name "\n\t"                                                                    \
        ".type " #name ", @object\n\t" #name ":\n\t"                                               \
        ".asciz " #literal "\n\t"                                                                  \
        ".size " #name ", . - " #name "\n\t"                                                       \
        ".previous\n\t");                                                                          \
    extern "C" char name[]

#endif // #ifndef OURS_VDSO_VDSO_EXPORT_HPP
