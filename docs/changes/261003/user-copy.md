# user-copy changes

## Title: UserPtr accepts only types that may cross the boundary as bytes (system-abi-surface step 4) (2026-10-03)

**Files**
- `kernel/lib/user_copy/include/user_copy/copy_allowed.hpp` — new: the `CopyAllowed` concept.
- `kernel/lib/user_copy/include/user_copy/user_copy.hpp` — `UserPtr` asserts `CopyAllowed` of its element type, where it asserted trivially copyable only.
- `kernel/lib/user_copy/BUILD.gn` — the new header listed.
- `kernel/lib/user_copy/test/user_copy_test.cpp`, `test/BUILD.gn` — `CopyAllowed`'s cases, as static_asserts.
- `BUILD.gn` — the user_copy host test is in the default build; nothing built it before.
- `.agents/tasks/system-abi-surface/DESIGN.md` — step 4 marked done.

**Symptom**
Latent, cannot be triggered today. `UserPtr<T>` accepted any trivially copyable T, including a struct with padding. Copying such a struct out to a program would hand it whatever the kernel stack held in the padding bytes. Every type copied today is an integer or an enum, so none has padding. But the next system call, object_get_info (K24), copies info structs, which is where such holes usually appear.

**Cause**
The objects:
- `UserPtr<T>`: the only way the kernel touches user memory. It copies the bytes of a T in or out, and nothing else.
- A struct the kernel fills on its stack and copies out, such as `struct Info { u32 kind; u64 base; }`.
- The padding the compiler puts between `kind` and `base`, 4 bytes, so that `base` is 8-aligned.

The events:
1. The kernel declares `Info info;` on its stack and sets both fields.
2. Nothing sets the 4 padding bytes, which keep whatever an earlier call left in that stack slot.
3. `copy_to_user(&info)` copies all 16 bytes, so the program reads the kernel's leftovers: an information leak.

Trivially copyable only says that copying the bytes makes a valid copy. It says nothing about which bytes are meaningful.

**Change**

```cpp
template <typename T>
concept CopyAllowed = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                      std::has_unique_object_representations_v<T>;
```

Unique object representations (equal values have equal bytes) is the condition that rules out padding, and a struct with its hole named as a field (`u8 padding1[4]`) passes it. Standard layout makes the fields sit where a C declaration of the same struct, in a program, expects them.

The host test pins the cases: `Padded` (16 bytes, refused), the same struct with the padding named (allowed), a polymorphic struct and one with fields in a base and a derived class (refused), one with its own default constructor (allowed), `float` (refused: +0.0 and -0.0 are equal with different bytes), and the types the system calls copy today (allowed). The test had never been built, so it is now in the default build, where building it checks every case.

**Reference**
Zircon's `user_ptr` asserts `is_copy_allowed` (`zircon/kernel/lib/user_copy/include/lib/user_copy/internal.h`), which is `is_trivial`, standard layout and unique object representations. This takes trivially copyable instead of `is_trivial`, as the design says:
- `is_trivial` also demands a trivial default constructor, which says nothing about whether bytes may be copied;
- `std::is_trivial` is deprecated in C++26, which this tree compiles as (`-std=c++2c`).

So a struct with a default constructor of its own is allowed here and not in Zircon.

**Verification**
- `ninja` on `out/vmo-snapshot`: the kernel, ring 3 and the host test build; every system call's pointer types pass.
- The host test runs: 6 tests passed. The concept's cases are checked by compiling it.
- Removing the unique-representations condition makes the test fail to build, on `!CopyAllowed<Padded>` and `!CopyAllowed<float>`, so it does catch the condition being lost.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).

**Deliberately not done**
- `UserPtr<void>`, which Zircon allows for untyped buffers. Nothing here needs it.
- The root `BUILD.gn` is reported "unsafe" by `scripts/format_gn.py` (its layout check cannot line it up), as it was before this change. The two lines added follow the file's layout.

**How it was found**
Step 4 of `.agents/tasks/system-abi-surface/DESIGN.md` (from the expert panel's ABI review).

**The user's prediction**
Asked which of the three conditions refuses `struct Info { u32 kind; u64 base; }`: "the third, because there is padding." Correct: 4 bytes of padding follow `kind`, `sizeof` is 16, and the struct is trivially copyable and standard layout, so only the third refuses it.

**Three questions you should be able to answer**
- What does a program learn from a padded struct the kernel copies out, and why does trivially copyable not prevent it?
- Why does a struct pass once its padding is named as a field, and what must the kernel then do with that field?
- Why is `float` refused, and why does that not matter for any system call here?
