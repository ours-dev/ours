# ustl changes

## ustl::mm::align_up and align_down are now ustl::round_up and round_down (2026-10-25)

**Files**
- `system/lib/ustl/include/ustl/mem/align.hpp` — the two functions renamed and moved from `ustl::mm` to `ustl`; `ustl::mm::is_aligned` stays where it was
- 27 callers in `kernel/` and `system/lib/launch/` — the new names; `kernel/mm/cpu_local.cpp` and `kernel/mm/vm_object_paged.cpp` also change their `using` declarations

**Symptom**
None, a rename the user asked for.

**Cause**
None, a rename. The standard library has no integer round-up. `std::align` moves a pointer inside a buffer, `std::is_sufficiently_aligned` (C++26) only tests a pointer, and `std::bit_ceil` rounds to a power of two, not to a multiple. So this helper stays in ustl, and the user chose the name `round_*` in `ustl` itself rather than `align_*` in `ustl::mm`.

**Change**
```cpp
// before
auto const rounded = ustl::mm::align_up(size, PAGE_SIZE);
// after
auto const rounded = ustl::round_up(size, PAGE_SIZE);
```

The bodies are unchanged. Not touched:

- The `align_up` helpers that obi's `PackageBuilder`, bootfs's builder, bootmem, multiboot, `phys/build_id_note.cpp` and a few tests define for themselves. They are not ustl's.
- Older change records, which are append-only.

**Reference**
None, this project's own library.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed, which include `launch`'s plan tests, the heaviest caller.
- `git grep "ustl::mm::align_"` outside `docs/` finds nothing.

**Deliberately not done**
- `is_aligned` stays in `ustl::mm`; the user asked for the two `round_*` only.
- Both functions assume the alignment is a power of two (a mask, not a division), and nothing checks it. That check is its own change.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `round_down(value, a)` work as `value & ~(a - 1)` only when `a` is a power of two, and what does it give for `a = 3`?
- Why is none of `std::align`, `std::bit_ceil` and `std::is_sufficiently_aligned` a replacement for `round_up` on a byte count?
- `round_up(value, a)` adds `a - 1` before masking; for which values does that wrap, and how do the callers in `VmoDispatcher::create_user()` rule them out?

## ustl::mm::is_aligned is now ustl::is_aligned (2026-10-25)

**Files**
- `system/lib/ustl/include/ustl/mem/align.hpp` — `is_aligned` moves into `ustl` beside `round_up` and `round_down`, and the `ustl::mm` namespace leaves the header
- 10 callers in `kernel/` — the new name; `kernel/arch/x86/kernel/bootstrap.cpp` changes its `using` declaration

**Symptom**
None, a rename the user asked for.

**Cause**
None, a rename. After the previous change one header held two namespaces, `round_*` in `ustl` and `is_aligned` in `ustl::mm`, for three functions that are one family. Nothing else in the tree lives in `ustl::mm`.

**Change**
```cpp
// before
if (!ustl::mm::is_aligned(offset, PAGE_SIZE)) {
// after
if (!ustl::is_aligned(offset, PAGE_SIZE)) {
```

`kernel/arch/x86/kernel/task/thread.cpp` also holds another session's uncommitted rename of `x64ContextSwitchFrame`. This commit takes only its `is_aligned` line: the commit was built in a temporary index from HEAD's copy of that file with the one line changed, so the other session's lines stay uncommitted in the working tree.

**Reference**
None, this project's own library.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- `git grep "ustl::mm::"` outside `docs/` finds nothing.

**Deliberately not done**
- The power-of-two check on the alignment, the next change.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `is_aligned(value, a)` need `a` to be a power of two, and what does it answer for `value = 6`, `a = 3`?
- How can one commit take one hunk of a file whose other hunks belong to someone else, without touching the index the other sessions share?
- Why is the header path `ustl/mem/align.hpp` left as it is although the namespace changed?

## round_up, round_down and is_aligned refuse an alignment that is not a power of two (2026-10-25)

**Files**
- `system/lib/ustl/include/ustl/mem/align.hpp` — `detail::require_power_of_two()`, called by `round_down()` and `is_aligned()`; `round_up()` goes through `round_down()`

**Symptom**
Latent, cannot be triggered today. Every caller passes a power of two. It would be triggered by any caller that someday passes another alignment, or 0. For example, `round_up(5, 3)` answers 5 instead of 6, with no error.

**Cause**
All three compute with the mask `alignment - 1`. That mask is "the bits below the alignment" only when the alignment has a single bit set:

- For 4096, the mask is `0xfff`. `value & ~0xfff` clears the low 12 bits, which rounds down to a multiple of 4096.
- For 3, the mask is `0b10`. `value & ~0b10` clears one bit in the middle, which is not rounding at all: `round_down(7, 3)` gives 5.
- For 0, the mask is all ones. `round_down` answers 0 for everything, and `is_aligned` answers true for everything.

Each answer has the right type and a plausible size, so nothing downstream notices.

**Change**
```cpp
// after
namespace detail {
    constexpr auto require_power_of_two(std::size_t alignment) noexcept -> void {
        if (!has_single_bit(alignment)) {
            __builtin_trap();
        }
    }
} // namespace detail

template <typename T>
constexpr auto round_down(T value, std::size_t alignment) noexcept -> T {
    detail::require_power_of_two(alignment);
    return static_cast<T>(value & ~static_cast<T>(alignment - 1));
}
```

- **Compile time:** in a constant expression the trap cannot be evaluated, so a constant misuse is a compile error.
- **Run time:** a misuse traps.
- **Cost:** an alignment known at compile time (`PAGE_SIZE`, `BLOCK`, `alignof`) folds the check away. The few run-time alignments (`CpuLocal::Dyn::allocate_locked()`, `ObjectCache`, `VmaSet::find_spot()`) pay one popcount and one branch.

`has_single_bit(0)` is false, so 0 is refused too.

Rejected options:

- A debug-only check: ustl has no debug switch, and its other checks (`Option::require_some()`, the intrusive `require()`) are always on.
- Rounding by division, which works for any alignment: it is slower on every call, and no caller needs a non-power-of-two alignment.

**Reference**
None, this project's own library.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- clang-tidy: nothing in the header.
- Before adding the check, I read every caller whose alignment is not a constant power of two. `CpuLocal::Dyn::allocate()` raises it to at least 8 and asserts a power of two. `ObjectCache` uses `alignof` or the caller's power of two. `LegacyMemory` checks for itself. `VmArea` passes `BLOCK` or `PAGE_SIZE`. So no current caller traps.
- Control: the same two files built against the header before and after, with alignment 3.

```text
before, static_assert(ustl::round_up(5U, 3) == 6):  static assertion failed, '5 == 6'
before, run time:                                   round_up(5, 3) = 5, exit 0
after,  static_assert(ustl::round_up(5U, 3) == 6):  not an integral constant expression (align.hpp, the trap)
after,  run time:                                   exit 133 (SIGTRAP)
```

**Deliberately not done**
- No host test of the trap: ustl's tests are plain `assert` programs with no death tests, and the control above covers it.
- The `align_up` helpers other libraries define for themselves (obi, bootfs, multiboot) are not checked here; they are not ustl's.

**How it was found**
None. It was raised while reviewing the rename: nothing stated the power-of-two assumption.

**The user's prediction**
None

**Three questions you should be able to answer**
- What do `round_down(7, 3)` and `is_aligned(6, 0)` answer without the check, and why does each look plausible?
- Why does the same `__builtin_trap()` become a compile error in a `static_assert` but a trap at run time?
- Why is the check free for `round_up(size, PAGE_SIZE)` but not for `round_up(base, align_bytes)` in `CpuLocal::Dyn::allocate_locked()`?
