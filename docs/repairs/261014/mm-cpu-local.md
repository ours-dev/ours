# mm-cpu-local outstanding repairs

## 1. CpuLocal::Current<T>::get() returns a copy of any T that fits in a register, class types included

**Severity**
Medium: no type in the tree takes the wrong path today, but the next small class type does, and silently if it is copyable.

**Symptom**
Latent. Deleting two unused `Ticks` fields from `time::TickSched` (`kernel/include/ours/time/tick_sched.hpp`) brought it to 8 bytes, and every `TickSched::get()` then failed to compile:

```text
kernel/arch/x86/lib/arch/include/arch/x86/fsgs.hpp:15:12: error: call to implicitly-deleted copy constructor of 'ours::time::TickSched'
```

It failed only because `TickSched` holds a `std::atomic<bool>` and cannot be copied. A copyable class of 8 bytes or less would have compiled, and `Self::get().mutate()` would have changed a temporary copy, never the CPU's own object.

**Cause**
`CpuLocal::Current<T>::get()` (`kernel/mm/include/ours/mm/cpu_local.hpp`, near line 699) chooses by size alone:

```cpp
if constexpr (sizeof(T) <= sizeof(usize)) {
    return CpuLocal::read(CURRENT_INSTANCE);   // a T, by value
} else {
    return CpuLocal::access(CURRENT_INSTANCE); // a T &
}
```

The by-value path exists for slots that hold a value, such as `Thread::Current`'s `Thread *`. It reads in one `%gs`-relative load, so a thread preempted and moved to another CPU cannot read the first CPU's slot (fce58bb8). Size stands in for "is a value", which holds for pointers and integers but not for a class that happens to be small. Whether `get()` returns the object or a copy of it then depends on the class's layout. `set()` makes the same test. A probe (`static_assert(sizeof(T) > sizeof(usize) || std::is_scalar_v<T>)` in `get()`, built with `-k 0`) found no class type on the value path today but `TickSched` with the fields removed.

**Change**
Take the value path for scalar types only, `std::is_scalar_v<T>` (pointers, integers, enums), in both `get()` and `set()`. A class type is then always reached by reference, whatever its size, as every larger one already is. Ruled out: padding `TickSched`, which routes around the layer instead of fixing it.

Once this is in, `kernel/time` can delete `TickSched::m_last_tick` and `m_next_tick`, which the user agreed to on 2026-10-14 and which this item blocks (they are marked `[[maybe_unused]]` until then).

**Reference**
None, this project's own design: the one-load read is fce58bb8's.

**Acceptance**
With the two `TickSched` fields deleted, the kernel builds; a host or kernel test shows `get()` on a small class type yields a reference (a mutation through it is seen by a second `get()`); `Thread::Current::get()` still compiles to one `%gs` load; both images boot.
