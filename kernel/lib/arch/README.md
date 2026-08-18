# libarch

## Description
`libarch` is the low-level architecture abstraction library. It declares the
architecture-neutral shape of CPU/platform facilities (system registers,
paging, interrupts, ticks, ...); the concrete per-architecture implementation
lives under `kernel/arch/<arch>/lib/arch` (e.g. `kernel/arch/x86/lib/arch`),
providing the same names under `include/arch/...`. Most headers here declare
signatures or forward-declare types that the architecture directory defines —
see `sysreg.hpp`/`io/register_base.hpp` for the system-register pattern and
`interrupt.hpp` for the plain-declaration pattern.

## Subdirectories
1. __macro__: this is used to compatible with assembly enviroment.

2. __paging__: A high-level abstraction for page table, provide a consistent API to operate it.