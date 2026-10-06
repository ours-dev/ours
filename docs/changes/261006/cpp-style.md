# cpp-style changes

## Title: every variable with static storage is UPPER_CASE without a g_ or s_ prefix, enforced by .clang-tidy and applied across the tree (2026-10-06)

**Files**
- `.clang-tidy` — `GlobalVariableCase`, `StaticVariableCase` and `ClassMemberCase` set to `UPPER_CASE`
- `.agents/skills/language/modern-cpp-development/SKILL.md` — the rule, its reason and its scope; the gotcha on constants extended to static storage; the `TA_GUARDED` example, which named `g_global_caches` (a variable that no longer exists), now names `TIMERS TA_GUARDED(LOCK)` in thread_sampler
- 53 C++ sources and headers and one assembly comment — 114 variables renamed: 66 at namespace scope, 33 static locals and 15 static data members. clang-tidy reported 104 of them outside the third-party trees; a search found the other 10 in files no compile database reaches

**Symptom**
`examples/profile/profile.cpp` declared `u8 g_dump[...]` at namespace scope, and clang-tidy accepted it. The user's rule is that global and static variables are UPPER_CASE, but it was written nowhere: neither `.clang-tidy` nor the C++ style skill nor CLAUDE.md had it. So nothing enforced it, and the tree used both spellings, 24 `g_` names beside 19 `constinit` UPPER_CASE ones. The style skill itself cited a `g_` global as its example of a guarded member.

**Cause**
`.clang-tidy` set `UPPER_CASE` only for constants (`GlobalConstantCase`, `ConstexprVariableCase`). A mutable variable with static storage fell through to `VariableCase`, which is `lower_case`, so `g_dump` passed. A rule that lives only in a conversation is a rule only the agent in that conversation follows. The next agent reads the config and the skill, and both said `lower_case` was fine.

The rule is worth enforcing because the case of a name is all a reader sees at the point of use. `slots[i] = ...` reads like a local, and the reader has to find the declaration to learn that every caller, and possibly every CPU, shares it. `SLOTS[i]` says so where it stands. A `g_` prefix says the same thing, which is why the user dropped it: on top of the case it would say it twice.

The rule covers every variable that outlives a call and is shared by every caller, mutable or not. A probe file run through clang-tidy with the new keys showed how the check sorts each kind, so the three keys were chosen from what it reported, not from the documentation:

| Declaration | clang-tidy's kind | Key that now applies |
|---|---|---|
| `int g_plain;`, `static int s_x;` and `namespace { int g_anon; }` at namespace scope | global variable | `GlobalVariableCase` |
| `constinit int g_c = 0;`, `thread_local int t;` and `extern "C" int c;` | global variable | `GlobalVariableCase` |
| `int const g_c = 1;` and `static int const s_c;` as a member | global constant | `GlobalConstantCase` (already set) |
| `static int s_m;` and `static inline int s_i;` as a member | class member | `ClassMemberCase` |
| `static int counter;` and `static int const limit;` in a function | static variable | `StaticVariableCase` |

**Change**
Three keys in `.clang-tidy`:

```yaml
  - key: readability-identifier-naming.GlobalVariableCase
    value: UPPER_CASE
  - key: readability-identifier-naming.StaticVariableCase
    value: UPPER_CASE
  - key: readability-identifier-naming.ClassMemberCase
    value: UPPER_CASE
```

The renames, done by tool rather than by hand:

1. clang-tidy, with only `readability-identifier-naming`, ran over each of the 409 translation units of `out/bringup.x86/compile_commands.json` outside the third-party trees, and exported its fixes.
2. Each fix gives the exact location of a declaration or a use. A script applied them, de-duplicated across translation units, with the new name computed as the old one without its `g_` or `s_` prefix, upper-cased: 389 edits in 48 files. In those files, comments citing the old name as `|name|` or `` `name` `` were updated too, and so was every whole-word use of a prefixed name, which cannot be an ordinary word.
3. Before the script wrote anything, every new name was looked for in its file. Two collided with a constant already there:
   - `userboot.cpp`'s `crashlog_rights` would have become the existing mask `CRASHLOG_RIGHTS`. It holds the rights the crashlog VMO was given, so it is `CRASHLOG_VMO_RIGHTS`, beside `CRASHLOG_VMO`.
   - `echo_server.cpp`'s array `slots` would have become the existing count `SLOTS`. It is `SLOT_STORAGE`, beside its neighbours `OUTGOING_STORAGE` and `ECHO_STORAGE`.
4. By hand, the places the fixes did not cover:
   - `logz4/src/domain.cpp`'s `section_start` and `section_end`. They are declared once per object format under `#if`, and clang-tidy gave no fix for them. Only the C++ name changed; each keeps its `LINK_NAME`, which is what binds it to the linker's section bound.
   - Ten variables in files no compile database here reaches:
     - `srat.cpp`'s `apicids` and `cpus`, compiled only with NUMA on;
     - `paging_test.cpp`'s `p2v`;
     - `abi_runtime_test.cpp`'s `object`;
     - `trace_examples.cpp`'s `a_lock`, `g_tracepoints`, `g_slots`, `g_buffer` and `g_fake_clock`;
     - `elf_parser_test.cpp`'s `image`.

     They were found by a pattern search for static locals and `g_` and `s_` names, and each was renamed only within its declaration's scope.
   - The comment in `user_copy.S` naming `feature_has_smap`. The assembly does not reference the symbol: the C++ side picks the variant.

Before checking a rename, every old name was looked for in assembly, linker scripts, GN and Python. The only real reference was that `user_copy.S` comment. The rest were ordinary words in comments and build files, such as `image`, `table` and `level`.

Rejected options:

- **`G_DUMP`, the prefix kept in upper case.** It is what clang-tidy's own fix produces. The user ruled the prefix out, since the case already says what it said.
- **Only the variables that had a `g_` prefix.** That would leave 87 lower-case statics that the new keys report, and the next run of clang-tidy would flag them all.
- **Renaming the third-party trees as well** (`vendor/`, `system/lib/lockdep`, `system/lib/concurrent`). They are hands-off, and 15 of the reported declarations are theirs. clang-tidy will keep reporting them.

**Reference**
None, this project's own rule. For comparison: Linux spells globals in lower case without a prefix and leaves the distinction to `static` and to file scope. Zircon writes `g` + CamelCase (`gBootOptions`) for globals and `k` + CamelCase for constants. Both mark static storage at the declaration only, which is what this rule moves to every use.

**Verification**
- The build of `out/bringup.x86` is clean, `-Werror` and thread-safety analysis on, with the kernel, the user programs and the host tools built.
- `scripts/format_cpp.py` was run on every changed C++ file.
- Re-running the survey after the change finds no declaration outside the third-party trees, against 104 before. Of the 19 it still reports, 15 are third-party and 4 were the `logz4` section symbols, fixed by hand after it ran. clang-tidy on `domain.cpp` then reported 0.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required). rel.log has `pc-ps2: key a`, `key left-shift` and `key A`.
- `kshell_run.py`: irq.test ok (69 checks), interruptline.test ok (58), interrupt.test ok (32).
- The host tests whose sources changed, rebuilt after the change, all pass:
  - logz4-test exits 0;
  - btree-extension-test, rbtree-augment-test and rbtree-instrument-test print "all checks passed";
  - process_launcher passes 15, vfs 42, fsio 34, route 32, sampler 23, loop 18 and svc 9;
  - boot-options-smoke and memory-config-smoke exit 0.
- Not compiled, so not verified beyond reading:
  - `srat.cpp`, which builds only with NUMA on;
  - `paging_test.cpp`, `abi_runtime_test.cpp`, `trace_examples.cpp` and `elf_parser_test.cpp`, which no build here reaches.

**Deliberately not done**
- The change records and task documents that cite old names (`feature_has_smap`, `g_global_caches`) keep them: change records are history and append-only.
- `ThreadSampler::CLAIMED` is a `static bool`. Whether a static data member is held to the flags-word rule for `bool` members is a separate question, and this change only renames.
- Code outside every compile database was searched by pattern, which finds `g_` and `s_` names and plain static locals. A lower-case namespace-scope variable without a prefix in such a file could still be missing. clang-tidy will report it once a build compiles that file.

**How it was found**
The user read `g_dump` in `profile.cpp` and asked whether it met the rule. Running clang-tidy on it showed no warning, and reading `.clang-tidy` showed why: the rule was not in it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which clang-tidy key decides the case of `static int counter;` inside a function, and which decides `static int const limit;` beside it?
- Why did the two renames that collided need a name of their own instead of the mechanical one, and what would `slots` -> `SLOTS` have done in `echo_server.cpp`?
- `SECTION_START` is renamed, yet the linker still finds it. What binds it to the section bound, and what would have broken had that been the C++ name?
