#pragma once

#include <ours/types.hpp>

namespace arch {

inline auto inb(u16 port) -> u8 {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

inline auto outb(u16 port, u8 value) -> void {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

} // namespace arch
