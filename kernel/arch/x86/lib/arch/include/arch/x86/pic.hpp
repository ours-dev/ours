/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef ARCH_X86_PIC_HPP
#define ARCH_X86_PIC_HPP 1

#include <arch/types.hpp>

namespace arch {
struct Pic {
    static auto disable() -> void;
    static auto issue(u8 irq) -> void;
    static auto remap(u8 pic1, u8 pic2) -> void;
    static auto wait_io() -> void;
    static auto mask(u8 vector) -> void;
    static auto mask_all() -> void;
    static auto unmask(u8 vector) -> void;
    static auto unmask_all() -> void;
};

} // namespace arch

#endif // #ifndef ARCH_X86_PIC_HPP
