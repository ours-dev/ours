# ustl outstanding repairs

## 1. `Result` and `Option` are never trivial for the purposes of calls, so every one is returned through memory

**Severity**
Low: a cost on every call that returns one, not a wrong result. How large is to be measured before anything changes.

**Symptom**
A function that returns `ustl::Result<u32, Error>`, eight bytes, does not return it in `%rax`. With the kernel's flags at `-O2`, clang 23 compiles

```cpp
[[gnu::noinline]] auto make(unsigned x) -> ustl::Result<unsigned, Error>;
```

so that the caller passes a hidden pointer in `%rdi` and the argument moves to `%esi`. The callee writes the result through the pointer (`setne 4(%rdi)`) and returns the pointer itself (`movq %rdi, %rax`). The caller must give every such call a stack slot and read the answer back from memory. `ustl::Option` is declared the same way, so it pays the same.

**Cause**
The Itanium C++ ABI passes and returns indirectly any type that is "non-trivial for the purposes of calls": one with a non-trivial copy constructor, move constructor or destructor, or with every copy and move constructor deleted. `Result`'s copy constructor, move constructor and destructor (`system/lib/ustl/include/ustl/result.hpp`, in both the primary template and `Result<void, E>`) are user-provided, because they must construct or destroy whichever union member is active. They are user-provided for every `T` and `E`, though, including the ones where the defaulted member would do the same thing. So even `Result<void, Error>` is never trivial. `Option` (`ustl/option.hpp`) is written the same way.

The cpp-style change of 2026-10-03 met the same fact from the other side. Its check for discarded values whose destruction `auto _` delays flagged five `Result<..., Error>` sites, though nothing in them has anything to destroy.

**Change**
Measure first, as CLAUDE.md asks of performance work, and bring the numbers to the user:
- which hot paths return a `Result` or an `Option` (system-call handlers, the handle table, mm);
- a benchmark in `benches/`: a non-inlined function returning `Result<u32, Error>`, against the same function returning a plain two-field struct;
- the `.text` size of both kernel images.

If it is worth doing, give both types conditionally trivial special members (C++20, P0848). Add a defaulted copy constructor, move constructor and destructor, each constrained on `T` and `E` being trivially copy-constructible, move-constructible or destructible, and constrain the user-provided ones on the negation. Do the same for the `Storage` union's members, and for assignment. Ruled out: making them trivial unconditionally, which would skip the active member's destructor for a `T` that owns something.

**Reference**
- The standard library does this for the same types:
  - `std::optional<T>`'s destructor is trivial when `T`'s is ([optional.dtor]);
  - `std::expected<T, E>`'s is trivial when both `T`'s and `E`'s are ([expected.object.dtor]);
  - their copy and move constructors are trivial on the same conditions.
- The Itanium C++ ABI defines "non-trivial for the purposes of calls", and passes and returns such a type through memory.

**Acceptance**
- `static_assert(std::is_trivially_copyable_v<ustl::Result<u32, E>>)` holds for an enum `E`, and its `Option` counterpart holds too, in ustl's host tests.
- The probe function above returns its result in `%rax` with no hidden pointer.
- Every ustl host test passes, and `ours-verify` passes.
- The benchmark's numbers are in the change record.
