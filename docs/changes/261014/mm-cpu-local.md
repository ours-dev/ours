# mm-cpu-local changes

## CpuLocal::Current<T> reads a value only for a scalar, never for a small object (2026-10-14)

**Files**
- `kernel/mm/include/ours/mm/cpu_local.hpp` — `Current<T>::IS_VALUE`, a scalar that fits in a register, chooses the path in `get()` and `set()`; comment
- `kernel/mm/cpu_local.cpp` — two `static_assert`s on `IS_VALUE`
- `docs/repairs/261014/mm-cpu-local.md` — item 1 fixed, the file removed with it

**Symptom**
Latent. Deleting two unused fields from `time::TickSched` brought it to 8 bytes, and every `TickSched::get()` stopped compiling:

```text
kernel/arch/x86/lib/arch/include/arch/x86/fsgs.hpp:15:12: error: call to implicitly-deleted copy constructor of 'ours::time::TickSched'
```

**Cause**
`get()` chose how to answer by size alone:

```cpp
if constexpr (sizeof(T) <= sizeof(usize)) {
    return CpuLocal::read(CURRENT_INSTANCE);   // a T, by value
} else {
    return CpuLocal::access(CURRENT_INSTANCE); // a T &
}
```

The by-value path is for slots that hold a value, `Thread::Current`'s `Thread *` and `PmNode::Current`'s `PmNode *`, so they are read in one `%gs`-relative load and a thread preempted and moved to another CPU cannot read the first CPU's slot (fce58bb8). Size was standing in for "is a value". It holds for pointers and integers, but not for a class that happens to be small: `get()` then returns a copy of the CPU's object. `TickSched` holds a `std::atomic<bool>`, so the copy was refused at compile time. A copyable class of 8 bytes or less would have compiled, and `get().mutate()` would have changed the copy and never the CPU's own object, with nothing to say so. Whether `get()` returns the object then depended on the class's layout, which any edit to its fields can change.

**Change**
One constant decides, and says what it means:

```cpp
static constexpr bool IS_VALUE = std::is_scalar_v<T> && sizeof(T) <= sizeof(usize);
```

`get()` and `set()` test `IS_VALUE` instead of the size. A class type is always reached by reference, as every larger one already was. A scalar too wide for one register would go by reference too, rather than trip `CpuLocal::read()`'s size assertion.

The test is two `static_assert`s in `cpu_local.cpp`, on a 4-byte struct and a pointer to it. They ask `IS_VALUE`, not the type `get()` returns. Asking `decltype(Current<S>::get())` instantiates `get()`, and with it a `CPU_LOCAL` instance of `S` in every CPU's unit: the first version did that, and the per-CPU area grew by 12 bytes.

Rejected: padding `TickSched` to more than 8 bytes, which routes around the layer instead of fixing it.

**Reference**
None, this project's own design; the one-load read is fce58bb8's.

**Verification**
- Built in `.worktrees/wip-proc-self` at 48c5787c plus only this change.
- The `.text` of both kernels (`vmours`, `vmours.with-tests`) is byte for byte the same as before the change: no type in the tree changed path, `Thread::Current::get()` included. The first version, asserting on `decltype(get())`, differed in 9 bytes, every one of them a RIP-relative reference to `__cpu_local_end` moved by the 12 bytes of the two instances it created.
- Mutation: `IS_VALUE` as the size test alone fails to compile, at `static_assert(!CpuLocal::Current<SmallObject>::IS_VALUE)`.
- `boot_check.py`: dbg and rel PASS, host tests 144 passed.

**Deliberately not done**
Deleting `TickSched::m_last_tick` and `m_next_tick`, which this unblocks: the next change, in `kernel/time`.

**How it was found**
The build error above, while deleting the two fields. The cause was in `Current<T>::get()`'s size test. A temporary `static_assert(sizeof(T) > sizeof(usize) || std::is_scalar_v<T>)` in `get()`, built with `ninja -k 0`, showed that no class type in the tree took the value path apart from the shrunken `TickSched`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must `Thread::Current::get()` be one instruction, and what goes wrong with two?
2. What would `get().mutate()` have done on a copyable 8-byte class before this change, and why would nothing have reported it?
3. Why does asserting on `decltype(Current<S>::get())` change the size of every CPU's per-CPU area, and asserting on `IS_VALUE` not?
