# wire-consteval: a method's ordinal is computed at compile time, and the codecs declare methods by selector alone

**Date**: 2026-10-30

## Delivered
Branch `wire-consteval`, worktree `.worktrees/wire-consteval`, two commits on `fa937cdc`:
- `f5fc847c` `feat(wire): a method's ordinal is computed at compile time`: `wire/sha256.hpp` (`wire::sha256`, FIPS 180-4, `constexpr`, any length, no heap), and in `wire/wire.hpp` `consteval wire::ordinal_of(selector)`, `consteval wire::method(selector, kind)`, `constexpr wire::all_distinct(methods...)` and `wire::ORDINAL_MASK`. `Method` keeps its three fields. Tests in `wire_test.cpp`. Change record `docs/changes/261030/wire.md`, first entry.
- the second commit, `refactor(wire): the codecs declare methods by selector alone`: the 19 declarations are `wire::method(...)`, each of the five headers has a `static_assert(wire::all_distinct(...))`, `ordinals_test.py` and every live sentence that named it are gone, `docs/repairs/261029/wire.md` item 2 is deleted (item 1 keeps its number). Change record, second entry.

The change record is `docs/changes/261030/wire.md`, not `261029`: no `261029/wire.md` existed to append to, and the directory is named for the day of the change.

### Lines of the echo and hello headers that changed
The guide quotes these. Both files lose the three-line comment above the declarations (their lines 51-53 before, 51-52 after) and the `.ordinal =` lines; nothing else of either changed. `git diff fa937cdc -- examples` shows them.

`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`, 128 lines before, 124 after:

| Before | After |
|---|---|
| 51-53 comment ("declared beside the selector its ordinal is hashed from ... ordinals_test.py recomputes the ordinals") | 51-52 comment ("each declared by its selector and kind alone: wire::method() computes the ordinal ...") |
| 56-60 `ECHO_STRING = wire::Method{ .selector, .ordinal, .kind }` | 55-56 `inline constexpr auto ECHO_STRING =` and `wire::method("ours.examples.echo/Echo.EchoString", wire::MethodKind::TwoWay);` |
| 63-67 `STOP = wire::Method{ ... }` | 59 `inline constexpr auto STOP = wire::method("ours.examples.echo/Echo.Stop", wire::MethodKind::TwoWay);`, one line of exactly 100 columns |
| (nothing) | 61-63 new: a two-line comment and `static_assert(wire::all_distinct(ECHO_STRING, STOP), "two Echo methods have the same ordinal");` |
| `struct EchoStringRequest` at 69 | at 65 |

`examples/hello/protocol/include/ours_examples_hello/ours_examples_hello.hpp`, 127 lines before, 124 after:

| Before | After |
|---|---|
| 51-53 comment | 51-52 comment, the same words as echo's |
| 56-60 `GREET = wire::Method{ ... }` | 55-56 `inline constexpr auto GREET =` and `wire::method("ours.examples.hello/Hello.Greet", wire::MethodKind::TwoWay);` |
| 63-67 `STOP = wire::Method{ ... }` | 59-60 `inline constexpr auto STOP =` and `wire::method("ours.examples.hello/Hello.Stop", wire::MethodKind::TwoWay);` |
| (nothing) | 62-64 new: the comment and `static_assert(wire::all_distinct(GREET, STOP), "two Hello methods have the same ordinal");` |
| `struct GreetRequest` at 69 | at 66 |

The two headers are still one rename of the other, with one formatting difference that the rename cannot avoid. Echo's `STOP` declaration is exactly 100 columns and stays on one line; the same line with `hello` for `echo` is 102 columns, so clang-format wraps it after `=`, as the committed hello has it. `ECHO_STRING` and `GREET` wrap in both. The guide's rename step has to say that the formatter (the hook, or `scripts/format_cpp.py`) runs after it, as it already must for `encode_greet_reply` and its neighbours, whose wrapping also differs from echo's.

## Verification
- Before the numbers were removed: a temporary `static_assert(wire::ordinal_of(selector) == typed_number)` for each of the 19, generated from the headers; the whole tree built, so all 19 matched. Changing one digit of one of them made the build fail in both toolchains, so the check was not vacuous. The asserts were then removed with the numbers.
- Tests: `wire` 22 (12 before, 10 new: 3 `WireSha256`, 5 `WireOrdinal`, 2 `WireMethod`), `ours_io` 21, `ours_process` 21, `ours_resource` 6, `ours_examples_echo` 7, all pass; the last four are unchanged tests. Every known answer is also a `static_assert`.
- `boot_check.py --out out/wire-consteval` before the first edit and after each commit: dbg 44 passed, 4 skipped, 0 failed, 0 error lines; rel 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines; host tests 151 passed. Identical counts all three times.
- Wire format: nine user executables (`init`, `echo_server`, `hello_server`, `shell`, `pc-ps2`, `userboot`, `routing_checks`, `profile`, `fault_probe`), stripped of debug information and build-id, are byte-identical (sha256) in a pristine build of `fa937cdc`, in a build of `f5fc847c` and in the final tree.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/wire-consteval`) on the eight translation units that include the changed headers, headers of the five libraries in scope: no diagnostics. A badly named probe file next to them was flagged, so the run was live.
- `lint_docs.py --changed` on every document touched: no violations.
- Mutations of the library, five, each caught at compile time by the `static_assert`s and, with those switched off, by the run-time tests alone: wrong last round constant, no top-bit clear, big-endian read of the digest, padding that spills at 57 instead of 56, an `all_distinct` that never reports. Table in the change record.
- Constant-expression steps (clang 23, smallest `-fconstexpr-steps` that compiles one `wire::method()`, same for the host and x86 user toolchains): 0 bytes 6,732; the longest selector in the tree, 34 bytes (`ours.examples.echo/Echo.EchoString`), 6,970; 55 bytes 7,117; 64 bytes 13,045; 255 bytes 32,417. The default limit is 1,048,576: the longest real selector uses 0.7 percent of it, a 255-byte one 3 percent.
- Cost, min of 5, `-fsyntax-only`, the build's compile command, a pristine build of `fa937cdc` measured at the same minute:

| Translation unit | Before | After |
|---|---|---|
| `ours_io.cpp`, x86 user toolchain | 141 ms | 151 ms |
| `ours_io.cpp`, host toolchain | 150 ms | 157 ms |
| a TU of only `#include <ours_io/ours_io.hpp>`, x86 | 98 ms | 110 ms |
| the same, host | 104 ms | 116 ms |
| `wire_test.cpp` (host), `-fsyntax-only` | 463 ms | 605 ms |
| `wire_test.cpp` (host), compile to object | 797 ms | 1049 ms |

About 3 ms of the `ours_io` difference is parsing `sha256.hpp` (measured on `f5fc847c`, where the headers still held their numbers), the rest is six hashes at one to two milliseconds each. 40 translation units include `ours_io.hpp`, about 100 include a codec header at all: an estimated 0.6 s of compiler time on a clean build of about 40 s. The `wire_test` rows are the new tests (35 hash evaluations and ten tests); the migration changed nothing in that unit. Not measured: a build with a cold ccache.

## Deviations from the design
None. There is no DESIGN.md or PLAN.md; the request was the brief given with the maintainer's decision of 2026-10-30. Two choices inside it:
- `sha256()` and `all_distinct()` are `constexpr`, not `consteval`, so that the tests can run them at run time too; `ordinal_of()` and `method()` are `consteval` as asked.
- The `wire_test.cpp` fixtures `TWO_WAY` and `ONE_WAY` keep their made-up ordinals, written out whole, so that the byte-level tests read in a hex dump; only the comment above them changed.

## Found along the way
- Eight of the 19 ordinals have the top bit of their raw hash set, so the clearing is exercised by the golden values themselves, not only by the dedicated test.
- `ours_resource`'s header comment says "the three Gets" for five: already `docs/repairs/261029/ours-resource.md` item 1. That item named `ordinals_test.py` twice; both mentions are gone from it.
- `docs/repairs/261029/wire.md` item 2 proposed running the script from `boot_check.py`. It is deleted, not fixed that way.

## Not done
- `docs/guide/`: `02-first-server.md` (Level 0, "fix two numbers", the failing script) and `03-protocols.md` ("Methods and their numbers", the `.ordinal = 0x0` workflow, the designated-initializer shape the script needed) still describe the old way. A following step syncs them, together with the lines of the echo and hello headers listed above.
- Mentions that stay because they are history, outside `.worktrees`, `prebuilt` and `docs/changes`/`docs/design`: `.agents/tasks/component-framework/{PLAN,RESULT}.md`, `.agents/tasks/microkernel-interrupts/{PLAN,RESULT}.md`, `.agents/tasks/server-guide-first-server/RESULT.md`, `.agents/tasks/server-guide-protocols/RESULT.md`, `.agents/tasks/server-guide-review/REVIEW.md`, `.agents/tasks/server-guide-review-2/REVIEW.md`; and under `docs/changes/` the older records of `261004`, `261005`, `261012` and `261029/examples-hello.md`. `docs/design/261004/component-framework.md:322` and `:404` still say that the tree has no SHA-256 in C++ and that a Python test (hashlib) recomputes the ordinals; they are left because `docs/design` was excluded from this change, and they are the first lines to correct in a follow-up.
