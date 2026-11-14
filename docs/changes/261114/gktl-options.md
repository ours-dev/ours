# gktl-options changes

## Declared options for console commands (2026-11-14)

**Files**

- `kernel/lib/gktl/include/gktl/options.hpp` — new: the declared-options builder, `gktl::Options` with typed `add<T>` handles, the fluent constraints (`defaults_to`, `required`, `choices` out of a static table, `parse` hook for number shapes plain digits cannot express, `help`), `parse(span)` / `parse(argc, argv)` / `parse({...})` over the tree's one grammar — `name` (a boolean set true) or `name=value` — returning `ustl::Result<Parsed, Rejection>`; `Parsed::get<T>/has/passthrough()`; the shared primitives `parse_u64` (promoted from ktest, which now delegates to it) and `parse_bool_strict` (the same strict word list `obi::CommandLineView::parse_bool` accepts for boot options, cited in its comment). All tables fixed-size to the console's 16-word cap, no heap; the whole header is free of kernel headers so the host suite compiles it.
- `kernel/lib/gktl/include/gktl/report.hpp` — new, kernel-side: the pinned error line a rejection maps to (`<cmd>: ERROR <rule> '<input>'`), the usage generated from the declarations (`usage: <cmd>` then one line per option: the fragment, the help, the default), and `reject()` = the pair a handler calls. The usage fragments build through `usage_word()` in options.hpp, pure and host-tested.
- `kernel/lib/gktl/test/options_test.cpp`, `kernel/lib/gktl/test/report_test.cpp` — new host suite `options-test`, 22 cases: the word grammar, the typed parses, every rejection fault, the pass-through, last-wins, the console-shape overload, the usage fragments, every fault's fixed phrase. The misuse asserts end in `ours::vpanic`, stood in by an `abort()` the binary defines once.
- `kernel/lib/gktl/BUILD.gn` — the new headers, the logz4 header dep report.hpp needs, the test target in `group("tests")` so boot_check's host phase discovers it.
- `kernel/lib/ktest/include/ktest/run_plan.hpp` — `parse_u64` now delegates to gktl's; the body is gone from here.

**Symptom**

None — a design request: the user (2026-11-14) asked that GKTL_COMMAND let a
command declare its options, cxxopts-style, so no command hand-parses argv
again. Eleven of the tree's twelve commands hand-parsed: five near-identical
digit loops (cpu, steal_policy, idle, lockup, steal_bench), strcmp verb
chains, a `starts_with("rounds=")` loop, argc-based overloading.

**Cause**

The command layer had no parse machinery of its own. Zircon's console gives
commands a pre-converted `cmd_args` union and its boot options parse through
an X-macro table; the closest in-tree precedents were ktest's
`RunPlan::from_words` and `obi::CommandLineView` — both shaped for one
consumer, neither reusable by a command. The parse code every command
carried was therefore written by hand, and hand-written parse code drifts:
the five digit loops agreed today and would drift the first time someone
fixed overflow handling in one of them.

**Change**

The mechanism, in three objects:

```cpp
auto steal_bench_command(int argc, char **argv) -> int {
    gktl::Options options{"steal.bench"};
    options.add<usize>("rounds").defaults_to(64).help("iterations per leg");
    options.add<bool>("csv").defaults_to(false);
    auto parsed = options.parse(argc, argv);
    if (parsed.is_err()) {
        gktl::reject("steal.bench", options, parsed.unwrap_err());
        return 1;
    }
    auto &values = parsed.unwrap();
    run_benchmark(values.get<usize>("rounds"), values.get<bool>("csv"));
}
```

- `add<T>` — the option's name and shape in one line. `T` is u64/usize (digits
  through `parse_u64`, or through the option's `parse` hook), bool (a bare
  word, or the strict `1/true/yes` vs `0/false/no` word list), or
  string_view (the raw word; `choices` validates it against a static table
  the command owns — a brace list's backing array would die with the
  statement that built it, which the first host run caught).
- `parse()` walks the words: unknown name → `UnknownOption` (unless the
  command declared `pass_through()`); a value that fails its shape →
  `BadValue` naming the option and quoting the value half; a value outside a
  Str option's choices → `BadChoice`; a required option the line did not
  carry → `MissingRequired`; a word with an empty name (`=x`) →
  `MalformedWord`. Duplicates: last wins. Bare word over a U64/Str option
  with a default means the default — that is how `shuffle` means its default
  seed; bare over one without a default is a BadValue. The answer carries
  every declared option's effective value plus, for a pass-through command,
  the words no declaration claimed.
- `reject()` prints the pinned line then the generated usage — the hand-written
  usage strings every command used to duplicate are gone, and a command's
  grammar now has exactly one description, the declarations.

**Reference**

The builder shape is cxxopts' (the vendored header is host-tool-only and
cannot link into the kernel), with the repo's own grammar and Result
conventions. The per-type parse registry follows Zircon's
`kernel/lib/boot-options` X-macro table in spirit — name, type, default, doc
in one row — realized as runtime handles because the user chose the runtime
builder over the schema-struct and X-macro shapes. The strict bool word list
is `obi::CommandLineView::parse_bool`'s, and the comment names it.

**Verification**

- `boot_check.py --out out/bringup.x86`: build ok; dbg PASS 46/5/0; rel PASS
  44/8/0 (7/7 required); host-tests 184/0 — options-test's 22 cases among
  them.
- cov_host on `kernel/lib/gktl`: 96.3% lines (337/350) — above the 95% gate.
- `python3 -I scripts/test_kshell_run.py`: green; `test_cov_kernel.py`: green
  after removing the stale `test_commands` case the migration round left
  behind (it tested a function the round had deleted).

**Deliberately not done**

- A range constraint (`at_least`/`at_most`) on number options. Two commands
  check their ranges handler-side where the range's meaning lives
  (`seconds takes 1 to 600`, `rounds takes 1 to 1000`); a constraint the
  framework could not phrase those as well as the handler does.
- Parsed storing raw words. The builder keeps parsed values and pass-through
  words, not the raw tokens; a command that wants the raw token (ktest's
  OPTION echo prints pass-through words only) can reconstruct what it needs
  from the Parsed, and the raw word for a declared option was never read by
  anything.

**How it was found**

None (the user's design request).

**The user's prediction**

None (a design request, not a measured change; the user set the direction —
the runtime builder over the schema-struct and X-macro shapes, all-command
migration over exemplar migration, verb-to-option grammar unification over
keeping the old word forms — answering the plan's questions, 2026-11-14).

**Three questions you should be able to answer**

- Why does a bare word over a number option mean its default while a bare
  word over a boolean means true?
- `choices({"a","b"})` with a brace list is a bug: what dies, when, and what
  does the acceptance check read instead?
- Why is the bool's word list strict where Zircon's boot options parse
  `on`/`off` leniently?

## The commands speak declared options (2026-11-14)

**Files**

- `kernel/commands/{crash,cov,idle,steal_policy,cpu,lockup,log}.cpp` and
  `benches/kernel/{include/bench/run.hpp,sched/steal_bench.cpp,time/preemption_deadline_bench.cpp,mm/large_page_bench.cpp}`
  — every command declares its words and parses through the builder.
- `scripts/{cov_kernel,test_cov_kernel,kshell_run,test_kshell_run}.py` — the
  drivers speak the new word forms.

**Symptom**

None — the migration the same request approved. The verb grammars were part
of the deal: the user chose verb-to-option reshaping knowing it breaks the
old word forms.

**Cause**

Same as the builder change's — hand-rolled parse code is the thing being
removed.

**Change**

One grammar, tree-wide: every word is `name` or `name=value`. The verbs
became options:

- `crash how=<panic|panic-locked|panic-smp>` (was `crash <how>`)
- `cov verb=dump` (was `cov dump`)
- `cpu verb=<status|offline|online> [cpu=<N>]` (was `cpu status|offline <cpu>|online <cpu>`)
- `lockup verb=<status|nmi|spin> [cpu=<all|N>] [ms=<N>]` (was `lockup status|nmi|spin <cpu> [<ms>]`)
- `log [verb=<dump|domains|level>] [domain=<d>] [level=<level|default>]` (was `log dump [<domain>] | domains | level [<domain>] <level>`; bare `log` still dumps the ring — the triage spelling the docs type)
- `idle.stats [seconds=N]` (was `idle.stats [<seconds>]`)
- `steal.policy [name=<knob> value=<N>]` (was `steal.policy [<name> <value>]`)
- the benches: `steal.bench [all] [csv] [rounds=N]` (words unchanged), `preemptdeadline.bench [all] [csv]` (words unchanged), `largepage.bench [all]` (words unchanged)

`bench::Run` no longer parses: its constructor takes the `all` flag the
command's own options answered, and `arguments_ok()` is gone — parsing
happened before the run was built. The five hand-rolled digit parsers are
deleted; their ranges live where the range's meaning lives (handler-side
checks that log the same refusals as before). The lockup judge in
kshell_run.py follows the new word forms. The `log` command keeps `reply()`
for its output — it sets the very floors that would silence its own reports,
so its output stays on the console sink — while its rejections go through
the same pinned lines as every other command's.

`dump-frame` stays as it was: it parses nothing.

**Reference**

The verb-to-option reshaping is the user's grammar-unification decision; the
log command's reply()/reject() split is the same property its change record
of 2026-09-24 established (a command that sets log floors reports through
the console sink).

**Verification**

- `boot_check.py` dbg+rel+host green after both migration commits; the
  migrated commands smoked on target in the new forms: `crash how=panic`
  (panic judged), `idle.stats seconds=2`, `steal.policy`, `cov verb=dump`,
  `steal.bench csv rounds=2`, `cpu verb=status`, `lockup verb=status`,
  `lockup verb=spin cpu=1 ms=300`, `log verb=domains`,
  `log verb=level level=warn`, `log verb=level domain=kshell level=default`,
  and the rejection path (`cpu nonsense` → pinned line + generated usage).
- `python3 -I scripts/test_kshell_run.py` and `test_cov_kernel.py` green.

**Deliberately not done**

- Keeping the old verb word forms. The user chose unification; the old forms
  are gone, and the drivers and docs that typed them follow.

**How it was found**

None.

**The user's prediction**

None (a design request; the user chose verb reshaping and all-command
migration, 2026-11-14).

**Three questions you should be able to answer**

- Why does the log command's output go through reply() while its rejections
  go through log::error?
- Why does bench::Run take a flag instead of argv now?
- `steal.policy name=x` without `value=` parses clean: why does the handler
  refuse it, and what would a `required()` on value have broken?

## The ktest runner speaks declared options (2026-11-14)

**Files**

- `kernel/lib/ktest/src/runner.cpp` — the command declares its options
  (`sel`, `filter`, `shuffle` with the seed hook, `repeat`, `shard`) plus
  `pass_through()` for the tests' own words; the death and list selections
  validate their shapes handler-side; the pinned `ktest: ERROR` lines are
  mapped from the builder's faults.
- `kernel/lib/ktest/include/ktest/run_plan.hpp` — `RunPlan` stays the data
  the run walks; `from_words`, `PlanFault`, `PlanRejection` and
  `runner_option` are gone (the builder's parse and rejection replace them);
  `parse_seed`/`Shard::parse` stay, as the seed hook and the handler-side
  composite parse.
- `kernel/lib/ktest/test/plan_test.cpp` — the from_words cases are gone;
  their grammar coverage lives in gktl's options-test, and the mapping the
  runner adds (fault-to-line, the all-only rule) is pinned by the console
  lines the host judges.
- `kernel/lib/gktl/include/gktl/options.hpp` — the Options class comment
  states where a long-hosting command's options belong.
- `scripts/kshell_run.py`, `scripts/test_kshell_run.py` — `ktest sel=list`,
  `ktest sel=all`, `ktest sel=death [filter=<glob>]`, the same runner option
  names.

**Symptom**

The first full `ktest sel=all shuffle` on the rebuilt image wedged: the
hotplug test's `take_cpu_offline` timed out past its deadline, its
assertions failed, and the run ended in a lockup report — deterministically,
in the same test, at the same position. Baseline, same order (identical
registry, same seed), passed.

**Cause**

The builder's frame cost. `gktl::Options` (16 descriptors), the parse's
`Result<Parsed, Rejection>` (a 16-slot value table plus a 16-slot
pass-through table) and the word array total a few kilobytes, and the whole
test run happens inside the ktest command's frame — every test body nests
below it on the same 16 KiB kernel stack (`mm::Stack::DEFAULT_STACK_SIZE`).
The ktest command's frame had grown past what the run beneath it could
afford, and the deepest timing-margined path — the hotplug offline — fell
over first.

**Change**

The options, their parse and the plan live in namespace-scope storage, the
way the runner's other tables do; the one-run-at-a-time guard (`RUNNING`)
makes the storage exclusive. The Options class comment in gktl now states
the rule: a command whose handler hosts long work keeps its options in
static storage, not on the frame. The new grammar:

- `ktest sel=<all|list|?|death|<filter>>` — the selection; bare `ktest` is
  still `sel=all` (what triage types and what docs record).
- `filter=<glob>` — the death run's filter; reserved on non-death runs.
- `shuffle[=SEED]`, `repeat=N`, `shard=M/N` — the runner's own, `sel=all`
  alone (the pinned line names `ktest sel=all` now).
- every other `name[=value]` — the tests', through pass-through, echoed as
  `ktest: OPTION` lines and read by the same `has_option`/`option`/
  `option_usize` accessors as before.

The pinned lines keep their shapes; the two that quoted the old grammar
quote the new one (`need "ktest sel=all"`, the usage line
`ktest sel=<all|list|?|death|<filter>> [filter=<glob>] [name=value ...]`).
The runner options are no longer recorded among the words the tests read:
they are the runner's, and the OPTION echo shows the tests' words only.

**Reference**

The static-storage rule is the runner's own established convention
(SHUFFLE_ORDER, the fixed tables of options.hpp — "one run at a time" is
what makes them safe); the change applies it to the builder the command now
declares with.

**Verification**

- `boot_check.py` dbg+rel+host green; host `ktest-test` 71/71 (the
  from_words cases' grammar coverage moved to gktl's options-test), host
  `options-test` 22/22.
- `ktest sel=all`: 83 ran, 80 passed, 3 skipped — twice, same numbers.
- `ktest sel=list`: 84 registry lines, order identical to baseline's.
- `ktest sel=all shuffle repeat=2 shard=0/1`: 166 ran, 160 passed, 6
  skipped — baseline's exact numbers.
- `ktest sel=Ktest.Matcher*`: 1 ran, 1 passed; the no-wildcard spelling
  correctly matches nothing.
- `ktest sel=death`: the death flow boots fresh per death test and the
  death test panics as expected.
- `ktest sel=all shuffle=5`: failed once (Timer.ForwardLatenessHoldOffAndRestartCancel,
  a timer-lateness margin test of the same family as the preempt-bounds
  repair), passed on retry and on baseline — a flake of that family, not a
  grammar regression; see `docs/repairs/261114/timer-forward-lateness.md`.

**Deliberately not done**

- Recording the runner's own words in the tests' option table. They are the
  runner's; the tests read the words the tests own.
- Keeping `from_words` beside the builder. Two parsers of one grammar would
  drift; the builder is the one parser now, and its coverage lives in
  gktl-test.

**How it was found**

The stack finding came from the first shuffle run's wedge and the
baseline-vs-worktree comparison that isolated it (same registry order, same
seed, same position, deterministic failure) — the frame was the only thing
the two images' run paths differed in.

**The user's prediction**

None (a design request; the ktest grammar supersedes the settled decision of
2026-11-05/07 in docs/design/261105/kernel-test-framework.md:139-154, which
the user chose with the full migration, 2026-11-14).

**Three questions you should be able to answer**

- Why do the runner's options and their parse live in namespace-scope
  storage, and what makes that safe?
- Why is `filter=` reserved on non-death runs?
- What does a bare `shuffle` mean, and through which builder rule?

## The C shape stops at run_command (2026-11-14)

**Files**

- `kernel/lib/gktl/include/gktl/command.hpp` — new: `gktl::Invocation`, the value a handler is invoked with (the command's own word plus the words after it, as views over the line); the `Callback` type becomes `auto (*)(Invocation const &) -> int`.
- `kernel/lib/gktl/src/command.cpp` — `run_command` builds the Invocation once from the console's count-and-pointers, which is now the only place that shape exists.
- `kernel/lib/gktl/include/gktl/options.hpp` — `parse(Invocation const &)` joins the parse overloads; `parse_word` loses its out-parameter and answers `ustl::Option<Word>`, per the codebase's no-out-param convention; the `parse(argc, argv)` overload is gone.
- the twelve handlers (`kernel/commands/*.cpp`, the three benches, `kernel/lib/ktest/src/runner.cpp`, `kernel/mm/pm_frame.cpp` — dump-frame ignores its words) — signatures take `gktl::Invocation const &`.
- `kernel/lib/gktl/test/options_test.cpp` — the parse_word cases read the Option; the console-shape case drives `options.parse(Invocation{...})`, which is now the real boundary shape.

**Symptom**

Review finding on the round's own API, named by the user: every handler
still received `int argc, char **argv` — the raw C shape the console hands
over — and each one immediately converted it into the builder's parse. The
C-ness the round removed from the parsing had survived in the signature
every command author writes.

**Cause**

The callback type predates the builder: `auto (*)(int argc, char **argv) ->
int` was registered the way Zircon's console registers its commands, and
when the builder arrived it sat beside that signature instead of replacing
it. The conversion it forced — twelve copies of the same
`options.parse(argc, argv)` call, each one a small reminder that the type
system had stopped at the console — was the C idiom wearing new clothes.

**Change**

The count-and-pointers shape is converted once, in `run_command`, into an
`Invocation` whose views alias the line for exactly the lifetime the raw
argv had; the callback hands the value on, and a handler's signature reads:

```cpp
auto crash_command(gktl::Invocation const &invocation) -> int;
```

The command's own word travels with the words (`invocation.command()`), so
nothing a handler used argv[0] for is lost. Alongside, `parse_word` stopped
taking an out-parameter: it answers `ustl::Option<Word>`, the shape the
review round of 2026-11-13 set for the rest of ktest and the reason its
`from_words` exists at all.

**Reference**

The review round's own finding — the environment walk's function-pointer-
plus-void* became virtual stage methods for the same reason: the boundary
where a C shape meets the object world belongs in one place, and nothing
downstream of it should see the C. The console itself keeps its `char *`
line buffer; that is the one side of the boundary C is honest about.

**Verification**

- `boot_check.py` dbg+rel+host green; host `options-test` 22/22, `ktest-test`
  71/71.
- On target: `lockup verb=spin cpu=1 ms=200` ok; `crash how=bogus` rejected
  with the pinned BadChoice line and the generated usage (the machine did
  not panic, which is the refusal working).

**Deliberately not done**

- Handing the handler an `Options` or a `Parsed` instead of an Invocation.
  The declarations are the handler's own, built inside its body — the
  framework cannot parse before the handler has declared what its words
  are. The Invocation is the honest middle: the line, split, value-typed.
- Replacing the `int` return with a Status. `run_command` discards it today;
  changing the contract it ignores is a separate question about what a
  command's failure means to the console, not a shape fix.

**How it was found**

The user's review of the round ("避免C Style").

**The user's prediction**

None (a shape review the user asked for; behavior pinned by the host suites
and the console grammar, both unchanged).

**Three questions you should be able to answer**

- Why must the Invocation's views alias the line, and what would copying the
  words cost?
- Why can the framework not parse before the handler runs?
- What makes run_command the right single place for the conversion, and
  what would go wrong if a handler could still take argc/argv?
