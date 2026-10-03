# cpp-style changes

## Title: A value dropped on purpose is written `auto _ = f(...);` (2026-10-03)

**Files**
- 90 C++ files across `kernel/`, `system/`, `examples/` and `benches/`: 405 statements `static_cast<void>(f(...));` or `(void)f(...);` become `auto _ = f(...);`, each keeping the reason comment it had. The largest are `examples/shell/shell.cpp` (120), the ustl containers and their tests (125), `system/service/init/init.cpp` (20) and `kernel/commands/*` (13).
- `system/lib/ustl/include/ustl/collections/intrusive/bstree.hpp` — one cast around a call that returns `void` removed: it discarded nothing.
- `kernel/lib/userabi/src/userboot.cpp` — `status = write_bootstrap(...); static_cast<void>(status);` becomes `auto _ = write_bootstrap(...);`, and the `status` before it is `const`, since nothing reassigns it any more.
- `system/public/ours/status.hpp` — the convention, in the comment on `Status`.
- `.agents/skills/language/modern-cpp-development/SKILL.md` — the rule, beside `[[nodiscard]]` and `[[maybe_unused]]`.

**Symptom**
None: a change of spelling. The tree wrote a deliberate discard as `static_cast<void>(...)`, the form step 2 of system-abi-surface used for the drops `[[nodiscard]]` on `Status` surfaced. The user preferred the C++26 placeholder.

**Cause**
The objects:
- `[[nodiscard]]`, on `Status` and `ustl::Result`: ignoring a returned value is a compile error unless the code says it is meant.
- Three ways to say so:
  - `static_cast<void>(f())`, the standard's own exemption ("unless explicitly cast to void");
  - `[[maybe_unused]]`, which applies only to a declaration, never to an expression, so `[[maybe_unused]] f();` is an error ("cannot be applied to a statement");
  - `auto _ = f();`, C++26's placeholder variable (P2169). Several may share a scope, none is warned about as unused, and none trips `-Wshadow`, all checked with this tree's clang 23 and flags.

The events:
1. Asked why the drops were not `[[maybe_unused]]`, the answer was that the attribute cannot mark an expression.
2. The user chose `auto _ = f();` over `static_cast<void>`.

**Change**

```cpp
// before
static_cast<void>(ort::debug_line("shell: channel checks passed"));
// after
auto _ = ort::debug_line("shell: channel checks passed");
```

The forms are not quite equivalent, and that is what the work was about. The cast destroys the result at the end of its statement; `auto _` keeps it alive to the end of the enclosing block. For a trivially destructible result (`Status`, an enum, an integer, a pointer, an iterator) nothing changes. For one that owns something, the destruction moves later.

How it was done: a workflow.
1. Four agents converted disjoint groups of files. They left these forms alone:
   - a cast of a plain variable or parameter (the unused-variable marker, 27 sites);
   - code in the hands-off `fair.cpp` (2);
   - `BitSet::clear()`'s `static_cast<void>(reset())`: `reset()` returns `BitSet &`, so `auto _ = reset();` would deduce `BitSet` and copy the whole set.
2. A check build proved which conversions keep a non-trivially-destructible object alive:
   - every `auto _ = E;` was rewritten as `auto _ = ours_discard_check(E);`, a helper constrained on `std::is_trivially_destructible`;
   - each of the 405 compiles was rerun so that every site yields exactly one diagnostic;
   - the 27 sites in ustl templates no target instantiates were forced from scratch translation units;
   - the tree was restored, and every file checked against its SHA-256.

   Result: 6 non-trivial sites, all kept as `auto _`:
   - `process_dispatcher.cpp`'s `Box<Handle>` is the last statement of its block, so it still closes the handle at the same point;
   - the other five are `ustl::Result`s whose payloads are trivial.
3. Three reviewers looked for lifetime, completeness and semantic problems, and a skeptic checked each finding. 14 findings, 8 confirmed:
   - the conversion had missed `kernel/commands/`, 13 sites;
   - the `void` cast in `bstree.hpp`;
   - the `userboot.cpp` discard hidden behind a variable;
   - the style rule missing from the skill.

   A last agent fixed them.

**Reference**
None in Zircon or Linux: both are C at their core and discard with `(void)`. The placeholder is C++26's `_` (P2169, "A nice placeholder with no name").

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: no error, no warning.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The shell's groups: 19 passed in dbg, 13 passed and 5 skipped in rel. 12 probes killed, launcher checks passed.
- Every host test binary the build produces: 104 run, none failed.
- The trivial-destructor check covered all sites converted in the first pass, 391 of 391: 385 trivial, 6 not (above).
- The tree this was verified in also held another session's uncommitted panic work, which is not part of this commit.

**Deliberately not done**
- `ustl::Result`'s destructor is user-provided whatever its payload, so even `Result<void, Error>` is not trivially destructible. Under the Itanium ABI that also makes every `Result` "non-trivial for the purposes of calls": it is returned through a hidden pointer to memory the caller provides, never in registers. A destructor that is defaulted when both payloads are trivially destructible (C++20's conditionally trivial special members) would fix both. That is a performance and ABI question for the user, to be measured first.
- `mutex.stress` on dbg once reported STUCK under host load. It needs about 18 s against its 20 s bound, and HEAD behaves the same, so it is not this change. It is a candidate repair item.
- The 27 unused-variable markers stay as they are. The skill's rule for those is `[[maybe_unused]]` on the declaration, a separate sweep if wanted.

**How it was found**
The user's question of why a drop is `static_cast<void>()` and not `[[maybe_unused]]`, and their choice of `auto _ =`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `[[maybe_unused]]` not mark a discarded call result, and what does `auto _` declare instead?
- When does `auto _ = f();` change what a program does compared with `static_cast<void>(f());`, and how was each such site found here?
- Why was `BitSet::clear()`'s `static_cast<void>(reset())` left as it was?
