# prelude changes

## macro_abi.hpp is gone: BIT()/KB()/MB()/GB()/TB() become the prelude literals (2026-10-10)

**Files**
- `system/public/ours/macro_abi.hpp` — deleted, and its line removed from `system/public/public_headers.gni`
- `kernel/**` (every other C++ header and source that used the macros) — every `BIT(n)` is now `n_bit` and every `KB/MB/GB/TB(n)` is `n_kb/_mb/_gb/_tb` (`kernel/include/ours/prelude.hpp`); the `#include <ours/macro_abi.hpp>` lines are dropped
- `kernel/mm/include/ours/mm/cfg.hpp`, `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/macro/{mmu,msr,system}.hpp` — assembly reads these, so they get plain numbers instead of literals (see Change)
- `kernel/mm/*.cpp`, `kernel/lib/heap/**`, `kernel/lib/arch/include/arch/paging/paging_traits.hpp`, `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp`, `kernel/commands/pm_*_test.cpp` — `BIT(order)` with a run-time or configured argument becomes `usize{1} << order`
- `kernel/mm/include/ours/mm/pm_frame.hpp` — a `static_assert` that keeps `NUMA_MAX_NODES_BITS` equal to the width of `NUMA_MAX_NODE`
- `scripts/qemu_debug.py` — a comment that named `GB(1)` now says 1 GiB

**Symptom**
Latent, cannot be triggered today. Two spellings of one thing coexisted: `16_kb` (added 2026-09-23, typed `usize`) and `KB(16)` (global macro, typed `unsigned long long`). A constant declared `constexpr static auto X = KB(16)` and one declared with `16_kb` therefore had different types, and a file had to be read twice to know which kind of constant it was looking at.

**Cause**
The header was written for the one reader that is not C++: early startup assembly. Everything in it is a macro so that the assembler's preprocessor can expand it, and the comment at its top says so ("Keep their expansion integer-only"). That requirement is real for the few headers assembly includes, but the header was then included by 18 C++ files that have no such need, and they paid for it:
- A macro has no type and no scope. `BIT` and `KB` are two- and three-letter names in the global namespace, claimed in every translation unit that reaches the header, which is the same trap `HZ` was (see the "Constants and configuration" rule in `modern-cpp-development`).
- The type was fixed at `unsigned long long` by the `ULL(x)` helper, not at `usize`. Anything declared `auto` from it deduced `unsigned long long`, which is not `usize` (`std::size_t`), so a value carried through `auto` was a different type from the `usize` that the functions taking sizes ask for.
- `BIT_WIDTH` was a 64-step chain of GNU `?:` conditionals so that it could be a preprocessor expression. Only `NUMA_MAX_NODES_BITS` had a user. `ZONES_SHIFT`, `NR_ZONES_PER_NODE_BITS` and `MAX_ZONES_BITS` were built on it with no user at all, `ZONE_MASK` was built on `ZONES_SHIFT` with none either, and they named `NR_ZONES_PER_NODE`, which is a C++ `constexpr` and not a macro, so assembly could never have expanded them.

**Change**
Three kinds of call site, three treatments.

A literal argument in C++ code becomes the prelude literal, which is the form `0_bit` and `16_kb` already had in the rest of the tree:

```cpp
// before
constexpr VirtAddr USER_STACK_TOP = USER_ASPACE_BASE + MB(16);
Shared = BIT(0),
// after
constexpr VirtAddr USER_STACK_TOP = USER_ASPACE_BASE + 16_mb;
Shared = 0_bit,
```

An argument that is not a literal has no literal form, because a user-defined literal only takes a number written in the source. These become an explicit shift on `usize`:

```cpp
// before
auto const nr_frames = BIT(order);
inline constexpr auto KMALLOC_MIN_SIZE = usize{BIT(KMALLOC_MIN_ORDER)};
// after
auto const nr_frames = usize{1} << order;
inline constexpr auto KMALLOC_MIN_SIZE = usize{1} << KMALLOC_MIN_ORDER;
```

A header that assembly includes cannot use either, because `2_mb` is not an assembler token. They get plain numbers. The integrated assembler skips the `U`/`L` suffixes on an integer (checked: `1UL << 47` and `0x8000000000000000UL` assemble to the right bytes) but does not lex a C++14 digit separator (`0x20'0000UL` is `unexpected token`), so these headers use unseparated hex with a trailing comment:

```cpp
// before
#define X86_MMUF_NOEXECUTABLE BIT(63)
#define USER_ASPACE_BASE MB(2)
#define PAGE_SIZE BIT(PAGE_SHIFT)
// after
#define X86_MMUF_NOEXECUTABLE 0x8000000000000000UL
#define USER_ASPACE_BASE 0x200000UL // 2 MiB
#define PAGE_SIZE (1UL << PAGE_SHIFT)
```

`cfg.hpp` loses `BIT_WIDTH`. `NUMA_MAX_NODES_BITS` is now written next to `NUMA_MAX_NODE` in each branch (6 for 32 nodes, 1 for one node), and `pm_frame.hpp` static-asserts it against `ustl::bit_width(NUMA_MAX_NODE)`, so editing one without the other stops the build. The four unused macros are deleted. Rejected: keeping a smaller `macro_abi.hpp` for the assembly-facing headers, because the only things assembly needs from it are `ULL` and `BIT`, and the assembler already accepts the suffixes without any help.

The value type of these constants moves from `unsigned long long` to `usize`/`unsigned long`, which is what every consumer already took.

**Reference**
None, this project's own design: the typed literals in `kernel/include/ours/prelude.hpp` (2026-09-23) are the replacement this change finishes adopting. The removed macros resembled Linux's `BIT()`; the project chose typed literals over that.

**Verification**
- Fresh `out/macro-abi` (`tools/set.py bringup.x86`), so no orphaned object could hide a missed use: build ok.
- `ours-verify`: dbg PASS (35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (33 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host tests 144 passed, 0 failed.
- `clang-tidy -p out/macro-abi` on the 17 changed `.cpp` files: 0 diagnostics on changed lines (412 older warnings in the same files, not touched). `format_cpp.py --check` is clean.
- Not measured: the generated code. Each replaced expression is the same shift on a constant or a variable, but the object code before and after was not compared.

**Deliberately not done**
- The macro constants that assembly reads (`PAGE_SIZE`, `X86_*`, the `aspace_layout.hpp` addresses) are still `#define`s. Turning them into `constexpr` would break `start.S`, `gdt.S`, `start64.S` and `bootstrap16.S`. `aspace_layout.hpp` is included by `start.S` but no assembly uses its macros today, so it could take the literals if that include goes away.
- No helper for "pages in an order" (`usize{1} << order`, about twenty sites in `mm/` and its tests). One owner for that expression is better, but it is a design change to `mm`, not part of retiring a header.
- `OURS_CONFIG_NUMA_MAX_NODES` still leaves `NUMA_MAX_NODE` undefined when the build sets it. Nothing sets it, and it was that way before this change.
- Old task and change documents that mention `macro_abi.hpp` are history and are not rewritten.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `x86/macro/mmu.hpp` not use `0_bit` when `clock_source.hpp` can, and what would the assembler say if it did?
- Why is there no literal form of `BIT(order)`, and what did the run-time sites become?
- Why does the `static_assert` for `NUMA_MAX_NODES_BITS` live in `pm_frame.hpp` and not next to the `#define` in `cfg.hpp`?
