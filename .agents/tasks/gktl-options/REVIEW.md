# gktl-options: review of the round (builder, migrations, Invocation)

**Date**: 2026-11-14

## Scope and method

The round's whole diff, baseline cb221550 through d2ff00b9 plus the review
fixes that follow it: `kernel/lib/gktl/**` (options.hpp, report.hpp,
command.hpp, command.cpp, both host suites), the twelve migrated handlers,
`kernel/lib/ktest`'s runner and plan, `bench::Run`. Two reviewers: an
independent agent over the four core files (fresh eyes, style authority
`modern-cpp-development`), and the author over the handlers, the runner and
the boundaries. Every High/Medium finding was verified by rereading the cited
code and, where it was reachable, reproduced or fixed with a test: the
default-value finding got a host case and a booted run of the command it
broke; the run_command bound got reread against every caller; the naming and
ternary findings were checked with clang-tidy
(`prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86`), which now
reports nothing on the files the round touched. `ASSERT`'s always-on status
in every build was verified in `system/public/ours/assert.hpp` before
trusting the misuse checks.

## What is done well

- The bounded-table discipline held everywhere it was checked:
  `options.hpp`'s slot arithmetic cannot exceed `MAX_OPTIONS` because words
  are capped before the loop and duplicates overwrite rather than append;
  pass-through capacity is bounded the same way.
- The `Value` union's active-member discipline: every write through a
  kind-asserted `defaults_to` or the kind-dispatched `read()`, every read
  through `read_as<T>` behind the always-on kind assert,
  `is_trivially_copyable` asserted.
- `Parsed`'s views aliasing the console line are documented three times and
  the lifetime is real (`run_command`'s word array outlives the callback;
  `unwrap()` answers a reference into the static `Result`).
- The refusal path is uniform: one pinned line per fault, usage generated
  from the declarations, and the command name now taken from the
  declarations too.
- `logz4` argument safety in the report layer (views and scalars only) was
  verified against its format front end.

## High severity

### H1. Declared defaults were never applied when the option was absent

`kernel/lib/gktl/include/gktl/options.hpp` (the post-parse pass applied only
`required`). `defaults_to` promises "the value the option reads as when the
line does not carry it", the usage line prints `(default N)` for it, and the
`Parsed` class comment states it — but `parse()` filled a default only for a
BARE word; an option the line never mentioned landed no slot and `get` fell
through to zero. Live in the tree: bare `idle.stats` read `seconds` as 0,
failed its own `seconds == 0` check, and printed an error whose generated
usage said `(default 5)`. The host suite missed it — the bare-word-default
and absent-without-default cases existed, absent-with-default did not.

**Fixed**: after the word loop, every `has_default` option the line did not
carry lands a slot carrying its declared default with the given-bit clear —
`get` reads the default, `has()` keeps "the line carried it", which is what
the runner's all-only rule reads. Host case
`ADefaultedOptionAbsentFromTheLineReadsItsDefault` pins it; booted evidence:
bare `idle.stats` now reports "over 5 s".

## Medium severity

### M1. run_command copied an unbounded argc into a fixed array

`kernel/lib/gktl/src/command.cpp` — the word-copy loop had no
`count != MAX_OPTIONS` bound. Today's only caller caps at the console's 16,
but `run_command` is the library's public boundary (the place "the C shape
stops"); any future caller passing more words would have walked past the
array before `parse()`'s own `TooManyWords` ever ran. **Fixed**: the loop is
bounded, with the truncation pointed at the filed repair
(`docs/repairs/261114/kshell-word-cap.md`).

### M2. The radix-tree insert result was discarded

`kernel/lib/gktl/src/command.cpp` — `auto _ = TREE.insert(*command)`. The
insert answers false for a name outside the encoder's alphabet (anything
beyond `[a-zA-Z0-9. -]`; the underscore a command author is most likely to
reach for) and for a duplicate name — both invisible in the section walk,
and a registry that silently omits one command is indistinguishable from a
typo in the shell. **Fixed**: `ASSERT(TREE.insert(*command), ...)` — the
first dispatch is where an authoring bug must speak.

### M3. Verb-scoped option validity was silently loosened in three commands

The old grammars refused extra words (`lockup status`, `lockup nmi`, `cpu
status` took none); the declared-options migration accepted `cpu=`/`ms=`
beside those verbs and ignored them. **Fixed**: `lockup` and `cpu` reject
the verb-scoped strays with the same error-plus-usage shape, restoring the
old discipline where the framework cannot express it.

### M4. reject() took the command name twice

`gktl::reject("crash", options, ...)` repeated the name the `Options` value
already carries — a handler that renamed its command could leave its error
lines calling the old name. **Fixed**: `report_rejection` / `print_usage` /
`reject` take the `Options` and read `cmd()` from the declarations.

## Minor and style

- Private members were written with trailing underscores
  (`command_`, `descriptor_`, `count_`...); the checked-in rule is `m_`
  (`.clang-tidy` PrivateMemberPrefix, the skill, and the sibling
  `radix_tree.hpp` in the same library). Renamed throughout. The public
  aggregate field `default_` became `default_value` — bare `default` is a
  keyword, and the vocabulary matches `sched_domain`'s `info.default_value`.
- Four ternaries (`given` selection, the parse-hook dispatch, two in the
  usage printer) rewritten as `if`/`else` per the style rule.
- `parse_u64` did not validate `base` (base 0 divides by zero); now
  `ASSERT(base == 10 || base == 16)`. The assert pulls `ours::vpanic` into
  host consumers of the primitive, so `ktest`'s host shim gained the same
  one-line stand-in `options-test` already carried.
- `report_rejection` dropped the malformed word it was handed; the
  MalformedWord line now quotes it like every other fault's.
- The 112-byte usage-buffer comment claimed more than the arithmetic
  supports for choices near the word cap; the comment now states the honest
  truncation and lets the caller size for its own declarations.
- `build_radix_tree`'s reserve failure latched `TREE_BUILT` and said
  nothing; it now logs one error line — a console where nothing answers and
  nothing explains is the worst shape a broken image can take.
- `report_test.cpp` drives `report_rejection(options, ...)` now, so the
  command-name-from-declarations rule is exercised by the suite.

## Teaching opportunities

### T1. "The line did not mention it" is not "the option has no value"

The H1 bug is the default-value contract split across three honest-looking
pieces: a bare word fills the slot, absence fills nothing, `get` answers
zero. Each piece is defensible alone; the contract lives in none of them.
The fix makes the declaration own its default at exactly one point (the
post-parse pass) and keeps given-ness as a separate bit — the same split
GoogleTest makes between `ValueIsPresent()` and `GetParam()`, and the same
reason Rust's `Option::unwrap_or` takes the default at the read, not the
parse.

### T2. A library boundary defends against the caller it has, not the callers it invites

`run_command` checked `argc <= 0` and null `argv` — the caller it had — and
not the array bound, the one shape its own documentation says it exists to
contain. The console's cap was doing the array's job. Zircon's console
tokenizer has the same coupling (its MAX_NUM_ARGS bound lives in the
tokenizer, and its commands trust it); the difference here is that the
boundary is a library function the next boot shim will call.

## Suggested follow-up order

1. Land the review fixes (this commit) — the tree is verified with them.
2. `docs/repairs/261114/kshell-word-cap.md`: the fix it proposes should
   cover both truncation sites (the console's splitter and run_command's
   bounded copy), per the comment now in command.cpp.
3. The timer-lateness shuffle flake repair stands as filed; the seed sweep
   it asks for is still worth running before choosing the remedy.
