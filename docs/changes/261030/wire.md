# wire changes

## Title: A method's ordinal can be computed from its selector at compile time, by a SHA-256 the compiler runs (2026-10-30)

**Files**
- `system/lib/wire/include/wire/sha256.hpp` — new: `wire::Sha256Digest` and `constexpr wire::sha256(std::string_view)`, FIPS 180-4, any message length, no heap, no mutable static state
- `system/lib/wire/include/wire/wire.hpp` — `consteval wire::ordinal_of(selector)`, `consteval wire::method(selector, kind)`, `constexpr wire::all_distinct(methods...)` and `wire::ORDINAL_MASK`; `Method` is unchanged
- `system/lib/wire/BUILD.gn` — `sha256.hpp` joins the `headers` target
- `system/lib/wire/test/wire_test.cpp` — known-answer, padding-boundary, golden-ordinal and distinctness tests, each as a `static_assert` and as a run-time test

**Symptom**
Latent, cannot be triggered today: all 19 ordinals in the tree are the hash of their selector (the golden test below recomputes each). The defect is in how they are kept right. Each codec header carries a selector and a 64-bit number typed beside it, two facts where one follows from the other, and the only check was `system/lib/wire/test/ordinals_test.py`, which nothing runs (`docs/repairs/261029/wire.md` item 2). A number mistyped, or copied from the method above it, builds, passes the codec's own host tests and boots; at run time it is an unknown method, which every server answers by closing the connection without a word.

**Cause**
The hash was never computed where it was needed because the tree had no SHA-256 in C++, so the number had to be typed and then checked from outside the build, by a script in another language that finds declarations by their text shape (`.selector` first, `.ordinal` right after). A check that is not part of the build is a check that is not run; a rule that must be remembered is one more thing a new protocol author gets wrong.

Why a compile-time SHA-256 removes the problem and not just the checking: the ordinal becomes a function of the selector, so there is nothing to disagree. `consteval` is what makes that safe. A `constexpr` function called in an initialiser may be evaluated at run time if the compiler decides it must, which would put the whole hash in every program that declares a method and run it before `main`. A `consteval` function can only be evaluated by the compiler: a selector that is not a constant is a compile error, and a `wire::Method` costs the program what any constant does: its 32 bytes of read-only data, if something takes its address, and nothing otherwise.

Two limits of the compiler matter for a hash. The step limit: clang stops a constant expression after `-fconstexpr-steps` operations (1,048,576 by default); SHA-256 costs about 6,500 per 64-byte block (measured, below), so a selector, which is one to five blocks, is far from it. And no heap: the padded tail of the message is built in a local `std::array<char, 128>`, which is why the function takes a `std::string_view` and not a growable buffer.

**Change**
`sha256()` follows FIPS 180-4 section 6.2: the whole 64-byte blocks of the message are compressed from the message itself, and the rest is copied into a buffer of two blocks, padded (a `0x80` byte, zeros, the length in bits as 8 big-endian bytes at the end of the last block) and compressed. The padding fits the rest's own block when the rest is 55 bytes or fewer and spills into a second block from 56; that boundary is the usual place for a hash like this to be wrong, so it has its own tests.

```cpp
// before: the number is typed beside the selector
inline constexpr auto READ = wire::Method{
    .selector = "ours.io/File.Read",
    .ordinal = 0x2348'9a6f'12eb'b0f0,
    .kind = wire::MethodKind::TwoWay,
};

// after (next commit): the number is the selector's
inline constexpr auto READ = wire::method("ours.io/File.Read", wire::MethodKind::TwoWay);
```

This entry adds the functions; the headers still carry their typed numbers, so that the next commit can prove, one declaration at a time, that the computed ordinal is the typed one before the typed one goes. `Method` keeps its three fields, so `header->ordinal == READ.ordinal` and `.selector` are unchanged for every user.

`all_distinct()` takes the methods as arguments and answers whether no two share an ordinal, so a codec header can `static_assert` it. It is `constexpr`, not `consteval`, so that the tests can also call it at run time on a made-up collision. Ordinals have to be distinct only within a protocol (the methods one server tells apart on one channel); the header decides which methods it passes.

Rejected: running `ordinals_test.py` from `boot_check.py` (the repair item's own proposal). It would catch a wrong number after it was typed, in a tool only some developers run, and keep the two facts. A GN action that generates the ordinals would put Python in the build graph for one hash.

**Reference**
The rule itself, the first 8 bytes of SHA-256 of the selector read little-endian with the top bit cleared, is component-framework design D20 (`docs/design/261004/component-framework.md`), taken from FIDL's ordinal generation in `fidlc`, which computes it in the compiler so that no one types it. Not checked against the Fuchsia source here, which is not in the tree. The padding and the round function are FIPS 180-4, sections 5.1.1 and 6.2.2.

**Verification**
- Build and tests: `ninja -C out/wire-consteval`, the `wire` host test is 22 tests (12 before, 10 new), all pass. Every known answer is also a `static_assert`, so a wrong hash fails the build before any test runs.
- Known answers: the three examples of FIPS 180-4 (the empty string, `abc`, and the 56-byte `abcdbcdecdef...nopq` that needs two blocks), compared byte by byte; hashes of the same alphabet-cycling message at 55, 56, 63, 64, 65, 119, 120, 128 and 255 bytes, computed with Python's `hashlib`; and the 19 ordinals that were typed in the codec headers (`GOLDEN`), taken from the headers before they change.
- Mutations, each applied to the library, the build run, and the library restored. The first column is what the `static_assert`s do (the build fails; the lines are the failing assertions of `wire_test.cpp`), the second what the run-time tests do alone, with the test's `static_assert`s switched off:

| Mutation | Build | Run-time tests that fail |
|---|---|---|
| last round constant `0xc67178f2` to `0xc67178f3` | fails at 13 assertions | FipsKnownAnswers, PaddingBoundaries, RunsAtRunTimeToo, EveryCodecOrdinalIsItsSelectorsHash, ReadsTheDigestLittleEndian, SelectorsAtThePaddingBoundaries, TopBitIsCleared |
| top bit not cleared (`return ordinal`) | fails at 4 | EveryCodecOrdinalIsItsSelectorsHash, ReadsTheDigestLittleEndian, TopBitIsCleared |
| digest bytes read big-endian | fails at 9 | EveryCodecOrdinalIsItsSelectorsHash, ReadsTheDigestLittleEndian, SelectorsAtThePaddingBoundaries, TopBitIsCleared |
| padding spills at 57 bytes instead of 56 | fails at 3 | FipsKnownAnswers, PaddingBoundaries, RunsAtRunTimeToo, SelectorsAtThePaddingBoundaries |
| `all_distinct` never reports a clash | fails at 3 | AllDistinct |

- Constant-expression steps, the smallest `-fconstexpr-steps` under which a TU holding one `wire::method()` compiles, found by bisection with the build's own compile command: 0 bytes 6,732; 34 bytes (`ours.examples.echo/Echo.EchoString`, the longest selector in the tree) 6,970; 55 bytes 7,117; 64 bytes 13,045; 255 bytes 32,417. The default limit is 1,048,576, so a 255-byte selector uses 3 percent of it and the longest real one 0.7 percent. The same on the host and the x86 user toolchain.
- clang-tidy on `wire_test.cpp` and `wire.cpp` with the headers of `system/lib/wire` in scope: no diagnostics (a probe file with a badly named function in the same directory was flagged, so the run is not vacuous).
- Cost of the parse alone, the headers still declaring their numbers by hand (min of 5, `-fsyntax-only`, the build's flags): `ours_io.cpp` x86 user toolchain 131 to 134 ms; a TU of just `#include <ours_io/ours_io.hpp>` 93 to 96 ms.
- Nothing reaches the target: the stripped, build-id-less user executables (`init`, `echo_server`, `hello_server`, `shell`, `pc-ps2`, `userboot`, `routing_checks`, `profile`, `fault_probe`) of this tree are byte-identical to a build of the commit before it. `boot_check.py`: dbg 44 passed, 4 skipped, 0 failed; rel 41 passed, 8 skipped, 0 failed, 6 of 6 required; host tests 151 passed; the same counts as before the change.

**Deliberately not done**
The 19 declarations are not migrated here; the next commit does it with the typed numbers still in place as a check. `ordinals_test.py` and the text that mentions it stay until then. `docs/guide/` is not edited: the tutorial's step for the ordinal is now unnecessary, and the guide is synchronised in a separate step.

**How it was found**
None. The maintainer decided on 2026-10-30 to compute the ordinal in C++ rather than run the script, after `docs/repairs/261029/wire.md` item 2 recorded that nothing ran it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `ordinal_of()` be `consteval` and not `constexpr`, and what would a `constexpr` one cost every program that declares a method?
- At which message length does SHA-256 need a second block for its padding, and why exactly there?
- Why does `all_distinct()` take the methods of one protocol, and not every method in the tree?
