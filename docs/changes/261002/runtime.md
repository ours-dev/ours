# runtime changes

## Title: User programs get an operator delete that refuses all but null, a __cxa_pure_virtual that traps, and no operator new (2026-10-02)

**Files**
- `system/lib/runtime/src/cpp_dependencies.cpp` — new: weak `operator delete(void *)`, `(void *, std::size_t)`, `(void *, std::align_val_t)` and `(void *, std::size_t, std::align_val_t)`, which do nothing for null and trap on anything else; a weak `__cxa_pure_virtual()` that traps.
- `system/lib/runtime/BUILD.gn` — the runtime's source set compiles it.

**Symptom**
The shell stopped linking once it called the K23 launcher:

```text
ld.lld: error: undefined symbol: operator delete(void*, unsigned long)
>>> referenced by binary.hpp:1659
>>>               elfload.program.cpp.o:(eftl::elf::BasicImage<eftl::elf::Traits64>::~BasicImage())
```

**Cause**
Under the Itanium C++ ABI, a class with a virtual destructor has two destructors in its vtable:
- D1, the complete-object destructor, which only destroys;
- D0, the deleting destructor, which destroys and then calls `operator delete` on `this`.

D0 exists so that `delete p` through a pointer to a base class frees the object with the right size and the right deallocation function, which only the dynamic type knows. The compiler emits D0 wherever it emits the vtable, whether or not the program ever deletes such an object. So the reference to `operator delete` comes from a declaration, not from any `delete` in the program: `virtual ~Binary() = default;` in eftl's base class (`system/lib/eftl/include/eftl/binary.hpp:19`). It makes `BasicImage`'s implicit destructor virtual, and its D0 is emitted with `BasicImage`'s vtable, which is why the linker points at the class head (`elf/binary.hpp:1659`). Clang 19 and later use sized deallocation by default in C++14 and later modes (earlier releases needed `-fsized-deallocation`), so D0 calls the sized form, `operator delete(void *, std::size_t)`, which is the symbol the linker missed.

A hosted program gets `operator delete` from its C++ runtime library (libc++abi or libsupc++). User programs here link none, and have no heap. The kernel has the same need, met by weak stubs in `//kernel/lib/libc/cpp_dependencies.cpp`. elfload ran only in the kernel until the launcher brought it to ring 3.

The same class brings a second need that today's -O2 build hides. Its base, `eftl::Binary`, is abstract, and the vtable of an abstract class has `__cxa_pure_virtual` in the slot of each pure virtual function: the function a call through an object still being built or already being destroyed lands in. At -O2 the optimizer drops `Binary`'s vtable, so nothing names it. The review compiled a reduction of the same shape with the repo's clang and found `U __cxa_pure_virtual` at -O0 and none at -O1 or -O2, so a build with `optimize_none` would fail to link.

**Change**

```cpp
namespace {
    auto refuse_any_but_null(void *const pointer) -> void {
        if (pointer != nullptr) {
            __builtin_trap();
        }
    }
} // namespace

[[gnu::weak]]
auto operator delete(void *pointer, std::size_t) noexcept -> void {
    refuse_any_but_null(pointer);
}
// and the unsized and the sized-aligned forms, the same
```

- Null is accepted, as a no-op: [new.delete.single] requires a replacement `operator delete` to take a null pointer.
- Anything else traps. There is no `operator new`, so no pointer can be one it returned; a delete that reaches here with one is a bug, and the program stops at it rather than carrying on.
- No `operator new` is defined. An accidental `new` stays a link error, which names the line that wrote it.
- Weak, as the kernel's are, so that an allocator linked in later replaces them without a duplicate symbol.
- Each sized form has its unsized partner, `(void *, std::align_val_t)` included: [new.delete.single] requires a program that defines the sized form to define the other.
- `__cxa_pure_virtual()` traps. The Itanium C++ ABI (3.2.6) leaves its behaviour open and expects it to end the program. The kernel's returns, into whatever follows the call, which is not a pattern to copy.

Rejected:
- A no-op on every pointer, as the kernel's: it would hide the bug a non-null pointer means.
- Making `eftl::Binary`'s destructor non-virtual or protected: a library changed to suit one environment. A deleted `operator delete` is no way out either: for a virtual destructor, the deallocation function must be found and not deleted, or the program is ill-formed ([class.dtor]).

**Reference**
The kernel's `//kernel/lib/libc/cpp_dependencies.cpp`. A Linux program gets these from libstdc++'s libsupc++ (`del_op.cc`, `del_ops.cc`, `del_opsa.cc`) or from LLVM libc++abi (`stdlib_new_delete.cpp`), where they call `free()`.

**Verification**
- The shell links with the launcher; `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- Nothing deletes anything and nothing calls a pure virtual function, so no path here has run. Each is a null check or a trap.
- Not built at -O0; the reduction above is the evidence that `__cxa_pure_virtual` is needed there.

**Deliberately not done**
- `operator delete[]`: only reachable through `new[]`, which is already a link error.
- An allocator for user programs.

**How it was found**
The link error above, building the K23 executor into the shell.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a class with a virtual destructor need `operator delete` when nothing ever deletes one?
- Why must a replacement `operator delete` accept a null pointer?
- Why does an abstract class need `__cxa_pure_virtual` at -O0 and not at -O2?
