# kernel-commands changes

## Title: The four console tests that include x86's private headers move from //kernel/commands to //kernel/arch/x86/commands, which x86's kernel target links when the shell is built in (2026-10-31)

**Files**
- `kernel/arch/x86/commands/{interrupt_line_test,irq_test,timer_share_test,ioport_test}.cpp` — `git mv` from `kernel/commands/`, not edited (git sees four 100% renames).
- `kernel/arch/x86/commands/BUILD.gn` — new: `source_set("commands")` with the four sources and one edge for each target that owns a header they include.
- `kernel/arch/x86/kernel/BUILD.gn` — imports `kernel_params.gni`, and `//kernel/arch/x86/commands` joins the `kernel` source set's deps when `kernel_shell_enabled`.
- `kernel/commands/BUILD.gn` — the four sources are gone; the comment on the `$ours_cpu` headers edge says what it is for now, and the header comment says which files belong in this directory.
- `kernel/kernel_params.gni`, `kernel/lib/kshell/README.md` — the sentences that say what `kernel_shell_enabled = false` leaves out of the link name the architecture's commands too.
- `.agents/skills/ours-verify/SKILL.md` — the `*.test` commands live in two directories.
- `docs/architecture/timer/callback-cpu-share.md`, `docs/repairs/261030/x86-apic.md`, `.agents/tasks/riscv-kernel/PLAN.md` — the live documents that named `kernel/commands/timer_share_test.cpp`, or said the x86-only commands were still in the riscv link, say where they are now.
- `docs/changes/261031/kernel-commands.md` — this entry.

**Symptom**
Latent on x86, and a hard stop on riscv. `//kernel/top` links `//kernel/commands` for every `$ours_cpu` when `kernel_shell_enabled` is true, which it is by default, so a riscv kernel compiles four files that include `<ours/arch/x86/apic.hpp>` (three of them), `io_bitmap.hpp` and `percpu.hpp` (`ioport_test.cpp`) and `<arch/x86/interrupt.hpp>`. The riscv probe, `ninja -k 0` on `kernel_riscv.dbg.1/obj/kernel/products/bin/vmours`, reports 125 failing compiles at HEAD, and these four are among them, each with `fatal error: 'ours/arch/x86/...' file not found`: `interrupt_line_test.cpp:67` and `irq_test.cpp:70` and `timer_share_test.cpp:59` on `ours/arch/x86/apic.hpp`, `ioport_test.cpp:34` on `ours/arch/x86/io_bitmap.hpp`. Nothing said so on x86, where the headers exist.

**Cause**
A source file is compiled for the build that links its target, not for the architecture it is written for, so where a file lives decides which builds can fail on it. These four were written one by one as kernel tests (`irq.test`, `interruptline.test`, `ioport.test`, `timershare.test`), put where tests went, and nobody had a second architecture to compile them for. `kernel/arch/x86/kernel/include/ours/arch/x86/` is private to x86 by design (`layering.md`: a header private to one architecture lives in `ours/arch/<cpu>/`), and a generic directory that includes from it can only build for x86.

GN did not catch it for a reason the previous seam entry recorded (`docs/changes/261030/kernel-arch-seam.md`): it checks includes per target, not per header. `//kernel/commands` has to depend on `//kernel/arch/$ours_cpu/kernel:headers` anyway, because the memory tests include `<ours/arch/aspace_layout.hpp>`, the header every architecture provides under that name. That one edge satisfies `gn check --check-system` for the x86-private headers too, since they are all owned by the same target. The edge was written for one kind of include and silently covered another.

Moving the files is what removes the problem, not a dependency or an `#if`. What decides whether a source is compiled is whether some target that is linked lists it, so a file that must only build for x86 belongs to a target that only x86's kernel target links. `CLAUDE.md` and `layering.md` already say so (the user's placement, 2026-10-30, over `kernel/commands/x86/`): `kernel/arch/<cpu>/commands`, linked by that architecture's kernel target under the same `kernel_shell_enabled` switch.

**Change**
Link, in x86's kernel target (`kernel/arch/x86/kernel/BUILD.gn`), not in `//kernel/top`:

```gn
# after the deps list of source_set("kernel")
  if (kernel_shell_enabled) {
    deps += [ "//kernel/arch/x86/commands" ]
  }
```

`//kernel/top` names `//kernel/arch/$ours_cpu/kernel`, so putting the edge on x86's side keeps `top` free of the word x86 and gives riscv's kernel target the same slot for its own commands. The new target's visibility is that one target (`visibility = [ "//kernel/arch/x86/kernel" ]`), so nothing else can link the tests by accident.

`//kernel/arch/x86/commands` lists seven deps, each the owner of a header the four files include directly:

| Dep | Header the four files include |
|---|---|
| `//kernel/arch/x86/kernel:headers` | `ours/arch/x86/apic.hpp`, `io_bitmap.hpp`, `percpu.hpp`, and `ours/arch/aspace_layout.hpp` (`ioport_test.cpp`, for the user address range) |
| `//kernel/lib/gktl` | `gktl/command.hpp` (`GKTL_COMMAND`) |
| `//kernel/lib/heap` | `heap/new.hpp` |
| `//kernel/mm:headers` | `ours/mm/gaf.hpp`, `ours/mm/vm_aspace.hpp` |
| `//kernel/object:headers` | `ours/object/*.hpp` (`interrupt_line_test.cpp`) |
| `//kernel/platform:headers` | `ours/platform/isa.hpp` |
| `//system/lib/logz4:headers` | `logz4/log.hpp` |

`//kernel/commands` had two more that the four do not need, `//kernel/lib/ktl` (none of them includes a `ktl/` header) and `//system/lib/lockdep` (`interrupt_line_test.cpp` includes the kernel's `ours/lockdep.hpp`, not lockdep's own headers); both are left out. How "needed" was decided: each dep was removed in turn, the four objects deleted so that they really recompile, and `ninja -k 0` run, then `gn check --check-system` on the new target. Only `//kernel/object:headers` fails the compile (`'ours/object/handle.hpp' file not found`, because its published config carries the include directory); `//kernel/arch/x86/kernel:headers`, `//kernel/platform:headers` and `//system/lib/logz4:headers` fail `--check-system` when removed (`Include not allowed`, one finding per include). `gktl`, `heap` and `//kernel/mm:headers` pass both tests today because another dep reaches them, and are kept because each owns a header the files include directly: the edge should not depend on which other dep happens to bring it.

In `//kernel/commands` the four sources are removed from the list. The edge `//kernel/arch/$ours_cpu/kernel:headers` stays, since `aspace_layout.hpp` is still included by the generic memory tests; its comment changes from a note that a per-target edge hides four x86-private includes, which are no longer there, to:

```gn
    # The memory tests include <ours/arch/aspace_layout.hpp> for the user
    # address range, a header every architecture provides under that name. The
    # tests that include one architecture's own headers are in that
    # architecture's commands directory (//kernel/arch/x86/commands).
```

Rejected:
- `kernel/commands/x86/` with an `if (ours_cpu == "x86")` in `//kernel/commands`: the user chose `kernel/arch/x86/commands` on 2026-10-30, and it would keep the CPU test in the generic target.
- Linking from `//kernel/top` under `if (ours_cpu == "x86")`: `top` would name an architecture, which every generic target is built to avoid.
- Leaving `timer_share_test.cpp` generic because it could use the generic rate getter: that is `docs/repairs/261030/x86-apic.md`'s item, still open. Today the file includes `ours/arch/x86/apic.hpp` for `apic_tsc_hz()`, so it is an x86 file; the brief moved all four.

**Reference**
Zircon keeps a command that only makes sense on x86 in the architecture's own sources, not in a generic console directory: `cmd_cpu` is registered in `zircon/kernel/arch/x86/arch.cc:305` (`STATIC_COMMAND("cpu", ...)`) and `cmd_idlestates` in `zircon/kernel/arch/x86/mp.cc:691`, both listed in `zircon/kernel/arch/x86/BUILD.gn` with the rest of the architecture's sources. Zircon gates them differently: `STATIC_COMMAND_START` (`zircon/kernel/lib/console/include/lib/console.h:54`, `:66`) expands to the command table under `CONSOLE_ENABLED` and to a dead function that references the callback otherwise, so the callbacks are compiled either way and the preprocessor makes them unreachable. Here a command is a file, and `kernel_shell_enabled` decides by GN whether the file is compiled at all, which is why the link edge is under `if (kernel_shell_enabled)`.

**Verification**
- Names. The x86 dbg `vmours` (`kernel_x86.dbg.1/obj/kernel/products/bin/vmours`) built at HEAD, before the change, against the one built after: `llvm-nm --defined-only` gives 3224 symbols each, and the sorted `type name` lists are identical (diff empty); sorted `name type size` from `llvm-nm -S` is identical too, so no symbol was added, dropped, renamed (an ICF-folded name would have shown) or resized. What does differ, all explained:
  - Addresses: 3956 lines of `llvm-nm -n` differ, because the four objects are now linked with x86's kernel target and no longer among `//kernel/commands`' objects, and everything between the two places shifts. The registration order in the command section shows it: before, `InterruptLineTest IoportTest IrqTest ... TimerShare` were interleaved alphabetically among the generic tests; after, `InterruptLineTest IoportTest IrqTest TimerShare` come first, then mm's `FrameDumper`, the benches and the rest. Lookup does not read that order: `gktl::run_command()` builds a radix tree keyed by command name from the records between `__commands_start` and `__commands_end` (`kernel/lib/gktl/src/command.cpp`). One reader does: `ours::syscall::command_list()` walks the same range in link order (`kernel/lib/syscall/src/command.cpp:98`), and the user shell keeps the first 2048 bytes of what it returns (`system/app/shell/shell.cpp`, `Registry`, whose own comment says the part that fits is shown). The registry text is about 4000 bytes (41 `GKTL_COMMAND` records in the sources, summed by a throwaway script), so the list was already cut, and with the four now first the commands that fall off its end are different ones: not measured, and nothing checks which are shown.
  - `.data` grows from 0x5086 to 0x508e bytes (+8). Every symbol has the same size, so the 8 bytes are alignment padding between data objects whose order changed.
  - `.debug_str` +378 bytes and `.debug_line_str` +36: debug names that embed a source path. Replacing the four `kernel/commands/<file>.cpp` paths with `kernel/arch/x86/commands/<file>.cpp` in the before string table gives the after table exactly (47550 strings each; 40 of them name one of the four paths, such as the type names of the lambda at `timer_share_test.cpp:254:28`). `.text`, `.rodata` and `.bss` sizes are unchanged (`llvm-size`: text 1142861 before and after).
- The four commands on the dbg image, `python3 scripts/kshell_run.py --out out/bringup.x86 <name>`, each exit 0: `irq.test: ok` (72 checks), `interruptline.test: ok` (58 checks), `timershare.test: ok` (3 runs, the thread never kept under 25.0 %), `ioport.test: ok` (14 checks).
- Shell-less build: `tools/set.py bringup.x86 --out out/r25-e7ee-noshell --args kernel_shell_enabled=false` configured, `ninja` built all 976 steps and linked `vmours`. It has no object under `obj/kernel/arch/x86/commands` or `obj/kernel/commands`, no string `irq.test`, `ioport.test`, `timershare.test` or `interruptline.test` (91 matches in the shell build), and one `COMMAND_` symbol, mm's `dump-frame`, which `kernel/top/BUILD.gn` keeps on purpose. `gn check` and the header-list check pass there. The directory was removed afterwards.
- riscv probe, `ninja -k 0` on `kernel_riscv.dbg.1/obj/kernel/products/bin/vmours`: 125 failing compiles before, 121 after, not the 122 the brief expected. The brief counted three of the four among the failures; all four were (the first errors are above: `ioport_test.cpp` fails on `io_bitmap.hpp`, the other three on `apic.hpp`, so a count by `apic.hpp` finds three). `diff` of the sorted failing objects: exactly the four, and no new failure.
- `gn check` plain: `Header dependency check OK` on `out/bringup.x86` and on `out/bringup.riscv`. `gn check --check-system`, parsed into distinct (file, line, include) pairs: x86 37 before and after, riscv 22 before and after, identical sets; none is in either commands directory.
- `header_lists.py` passes on both directories: x86 `8 targets, 8 include roots, 106 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed`, riscv `... 81 headers ...`. (The new target is a source set with sources, so it is not a header interface and adds no list to check.)
- x86 `boot_check.py`: build ok; dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 8 include roots, 106 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed · 2.1s; exit code 0.
- riscv `boot_check.py --out out/bringup.riscv`: riscv PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi; headers PASS 8 targets, 8 include roots, 81 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed · 1.5s; exit code 0.
- `format_cpp.py --check` on the four moved files: no changed lines. `clang-tidy -p out/bringup.x86` on each: 121 to 126 diagnostics apiece, every one in a header (ustl, mm, gktl, libarch) and none in the four files, which are byte-identical to what they were.
- `format_gn.py --check` on the three BUILD.gn files and `lint_docs.py --changed` on the documents: no violations.

**Deliberately not done**
- `idle.cpp` (`ArchIdleThread::waits_in_mwait`) stays in `kernel/commands`: it is a separate item, not part of R25.
- No change to the command registry, `GKTL_COMMAND` or the shell: no file shares a helper header, a name or a registration with another file of `kernel/commands` (each of the four includes only `<...>` headers from other libraries and registers one command, `commands::InterruptLineTest`, `IrqTest`, `TimerShare` and `IoportTest`, in a namespace of its own), and nothing in `scripts/` or the skills names a command by position. The only reader of the registry's order is the user shell's list of commands, described under Verification.
- `//kernel/commands`' other deps are not pruned for what the four no longer need (the edge to `$ours_cpu`'s headers stays; `//kernel/platform:headers`, `//kernel/object:headers` and the rest may now be needed by fewer files). That is a different question from where these four live.
- `timer_share_test.cpp` is not changed to call the generic rate getter: `docs/repairs/261030/x86-apic.md` item 1 does that, and it is now an item about one x86 file calling another.
- `.agents/tasks/arch-platform-seams/DESIGN.md` is not edited: it still reads "R25 ... a new `kernel/commands/x86/` (or `kernel/arch/x86/commands`)", and is brought up to date in one pass later. The riscv PLAN's open decision row for step 1b still lists "gate the x86-only commands by arch" as an option, and its step 7 still says the same; both are the user's decisions, and the gate is now done at the link, so the choices left in that row are about kshell and the generic commands.
- riscv's own `kernel/arch/riscv/commands`: there is no riscv-only command today.

**How it was found**
From the arch-platform-seams work, item R25. The leak was recorded by `docs/changes/261030/kernel-arch-seam.md` (the per-target edge, and the riscv probe's failing compiles with `ours/arch/x86/apic.hpp` named); this change moves the files. The riscv probe was read for the first error of each of the four objects before and after.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `//kernel/commands` depended on `//kernel/arch/$ours_cpu/kernel:headers` for `aspace_layout.hpp`. Why did that one edge also hide four tests' includes of x86-private headers from `gn check --check-system`, and why does moving the files, not a better dependency, make riscv stop compiling them?
- The edge to `//kernel/arch/x86/commands` is in x86's kernel target and not in `//kernel/top`. What would each choice put in `//kernel/top`'s BUILD.gn, and which one is the same for every `$ours_cpu`?
- The symbol lists before and after are identical, yet every address moved and `.data` grew by 8 bytes. What decides where an object lands in the link, and why is padding the only thing that can account for the 8 when every symbol has the same size?

## Title: //kernel/arch/x86/commands lists an edge for every target that owns a header its four files include, as its comment says, instead of the seven it took from //kernel/commands (2026-10-31)

**Files**
- `kernel/arch/x86/commands/BUILD.gn` — four edges added (`//kernel/arch/x86:headers`, `//kernel/lib/arch:headers`, `//system/lib/ustl:headers`, `//system/public`); every edge has a comment naming the headers it is for; the comment on the list states the rule and its one exception.
- `docs/changes/261031/kernel-commands.md` — this entry.

**Symptom**
The comment on the list read "One edge for each target that owns a header the four files include", and the entry above and the body of `3135d44a` say the same. Matching every `<...>` header the four files include directly to the target that lists it (`gn refs`) against the target's deps (`gn desc ... deps`) finds four owners that were not edges:

| Owner that was not an edge | Headers the files include from it |
|---|---|
| `//kernel/lib/arch:headers` | `arch/spin_hint.hpp` (all four), `arch/tick.hpp` (`timer_share_test.cpp`), `arch/auto_interrupt_disabler.hpp` (`ioport_test.cpp`) |
| `//kernel/arch/x86:headers` | `arch/x86/interrupt.hpp` (`interrupt_line_test.cpp:88`) |
| `//system/lib/ustl:headers` | `ustl/option.hpp` and `ustl/rc.hpp` (`ioport_test.cpp:51-52`) |
| `//system/public` | `ours/interrupt.hpp`, `ours/port.hpp`, `ours/syscall.hpp` (`interrupt_line_test.cpp:69,78,79`) |

The review of `3135d44a` named the first three; the fourth turned up when the list was rebuilt from `gn refs`. `gn desc` of the target already showed their `headers.config`s among its configs, though nothing on its list named them. Nothing failed: `gn check` and `gn check --check-system` pass either way (37 and 22 pairs of findings before and after, the same sets).

**Cause**
GN lets a target include the headers of any target it reaches through `public_deps`, not only of its direct deps. The one edge to `//kernel/arch/x86/kernel:headers` reaches all four owners that way (`gn path` prints the chain): `kernel:headers` has `//kernel/arch/x86/lib/page_tables` as a public dep, which has `//kernel/arch/x86:headers`, `//kernel/lib/arch:headers` and `//system/lib/ustl:headers` as public deps, and `//kernel/lib/arch:headers` has `//system/public`. So the checker was satisfied by a chain that has nothing to do with these four files, and the same goes for gktl, heap and `//kernel/mm:headers`, which the entry above kept for exactly that reason.

The compile does not need the edges either. The kernel toolchain puts the include directories of lib/arch, gktl, arch/x86, arch/x86/kernel, mm, platform, ustl, logz4 and a few more on every kernel target (`prefix_configs` in `build/switch/kernel_switchset.gni`), and `//kernel/products:kernel.config` adds `//kernel/include` and `//system/public`. Only `//kernel/object:headers`' directory comes from an edge. So an edge in this kernel is a declaration read by `gn check` and nothing else, and "needed to build" is the wrong test for one.

The entry above used two tests on one list. It removed each dep in turn and asked whether the build or `--check-system` failed (the "needed" test), and dropped `ktl` and `lockdep` on that. It also kept `gktl`, `heap` and `//kernel/mm:headers`, which pass that test, on the rule "owns a header the files include directly, and the edge should not depend on which other dep happens to bring it" (include-what-you-use). The second rule was applied to three of the seven and to nothing else. The seven were `//kernel/commands`' twelve minus what the four did not need, so which targets were on the list was inherited, and the targets `//kernel/commands` never named (lib/arch, x86's arch headers, ustl, public) were off it for the same reason. The rule the comment wrote down and the list disagreed.

The entry above also says `//kernel/commands` "had two more" deps than the new target. It had five that the new target lacks: `ktl` and `lockdep`, which no comment ties to a file, and `//kernel/lib/lockup_detector:headers`, `//kernel/lib/user_copy` and `//system/lib/profdata`, which belong to other files' commands (`lockup status`, `user_copy_test.cpp`, `cov dump`).

**Change**
The list now holds one edge per owner, and each edge says which headers it is for:

```gn
# before
  # One edge for each target that owns a header the four files include.
  deps = [
    # <ours/arch/x86/apic.hpp>, io_bitmap.hpp and percpu.hpp, and
    # <ours/arch/aspace_layout.hpp> for the user address range.
    "//kernel/arch/x86/kernel:headers",
    "//kernel/lib/gktl",
    "//kernel/lib/heap",
    "//kernel/mm:headers",
    "//kernel/object:headers",
    "//kernel/platform:headers",
    "//system/lib/logz4:headers",
  ]

# after: the same seven, plus four, each with its own comment
  deps = [
    "//kernel/arch/x86:headers",          # <arch/x86/interrupt.hpp>
    "//kernel/arch/x86/kernel:headers",
    "//kernel/lib/arch:headers",          # <arch/spin_hint.hpp>, tick.hpp, auto_interrupt_disabler.hpp
    "//kernel/lib/gktl",
    "//kernel/lib/heap",
    "//kernel/mm:headers",
    "//kernel/object:headers",
    "//kernel/platform:headers",
    "//system/lib/logz4:headers",
    "//system/lib/ustl:headers",          # <ustl/option.hpp>, rc.hpp
    "//system/public",                    # <ours/interrupt.hpp>, port.hpp, syscall.hpp
  ]
```

The comment on the list now defines "owns": the target that lists the header or publishes the include directory it is under, or the library that exports that target (`gktl` and `heap`, whose `:headers` targets list `gktl/command.hpp` and `heap/new.hpp`). It says an edge stays when another reaches the same target through `public_deps`, and names the one exception, the headers under `//kernel/include`, which no target owns.

Rejected:
- Keeping the seven and rewording the comment to what the compile and `--check-system` need. It is honest, and it is the shape of `//kernel/commands`' list. It makes the list depend on other targets' deps (take `page_tables`' public deps away and an edge becomes necessary), which is the property the entry above argued against; and for these headers nothing is "needed" to compile, so `gktl`, `heap` and `//kernel/mm:headers` would have needed some other explanation.
- Editing `//kernel/commands`' list or any other target's to the same rule. That is a rule for the whole kernel graph and not for this target; it is returned as a question.
- Naming `//kernel/lib/gktl:headers` and `//kernel/lib/heap:headers` in place of the libraries. The libraries are link edges the entry above chose and the link must not change.

**Reference**
None, this project's own build graph. The rule being applied is GN's include check: `gn help check` says a target may include the headers of its direct deps and of what they reach through `public_deps`.

**Verification**
- `gn desc` of the target: 11 deps, the 7 before and the 4 above. `gn refs` over every `<...>` include of the four files: each owner is now an edge, except `gktl:headers` and `heap:headers` (their libraries are), `//kernel/mm:headers`, which lists none of its headers (`docs/repairs/261030/build-headers.md` item 1) and is an edge as the publisher of its directory, and the headers under `//kernel/include`, which no target lists.
- `gn gen` on both directories, `.ninja` files compared (713 for x86 and 707 for riscv, counted with `find` including orphans): only x86's two `commands.ninja` (dbg.0 and dbg.1) changed, and in it the target's `linkdeps` edge holds the same 25 order-only inputs in a different order (every added target was already reached); riscv's graph does not contain the target, and none of its files changed. `compile_commands.json`: the same 752 entries as a set before and after, so no compile command, the four objects' included, changed.
- `gn check`: `Header dependency check OK` on both directories. `gn check --check-system`, as distinct (file, line, include) pairs: x86 37 and riscv 22 before and after, identical sets.
- x86 `boot_check.py`: build ok (0.5 s, nothing to recompile); dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 5.0s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 8 include roots, 106 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed · 1.9s.
- riscv `boot_check.py --out out/bringup.riscv`: riscv PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi; headers PASS 8 targets, 8 include roots, 81 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed · 1.6s.
- `format_gn.py --check` on `kernel/arch/x86/commands/BUILD.gn` and `lint_docs.py --changed` on this record: no violations.

**Deliberately not done**
- No other target's deps are changed. `//kernel/commands` has no edge to `//kernel/lib/arch:headers` or `//system/lib/ustl:headers` although its files include `<arch/...>` and `<ustl/...>` headers, and so do many more targets. Whether every kernel target should list its header owners is a decision about the graph's convention; it is put to the user.
- `//kernel/include` has no owning target, so there is no edge to add for `ours/cpu.hpp`, `ours/task/thread.hpp`, `ours/time/clock.hpp` and the rest of what the four include from it.
- No dep is removed. `gktl`, `heap` and `//kernel/mm:headers` are the edges the rule keeps.

**How it was found**
The independent review of `3135d44a` read `gn desc` of the new target, saw `//kernel/lib/arch:headers.config`, `//kernel/arch/x86:headers.config` and `//system/lib/ustl:headers.config` among its configs with no matching dep, and compared that with the comment. For the fix, a throwaway script resolved each included header against the target's `include_dirs`, ran `gn refs` on the file to find the target that lists it, and printed the ones that were not deps; `gn path` from `//kernel/arch/x86/kernel:headers` to each of them gave the chain.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `gn check` passed with four owners missing from the list. Which path through the graph satisfied it, and what would you change to make it start failing?
- The four files compile with the include directories of lib/arch and ustl on their command line, with no edge to either. Where do those come from, and what does that make an edge to a `:headers` target in this kernel?
- The entry above applied two tests to one list. Which two, and why does each give a different list?

## Title: The user shell's list of kernel commands, which the move of four tests reordered, is cut at 2048 bytes: measured, and written up as a repair item (2026-10-31)

**Files**
- `docs/repairs/261031/shell.md` — new: item 1, the buffer, the missing offset and the link order, with the options and an acceptance.
- `docs/changes/261031/kernel-commands.md` — this entry.

**Symptom**
The first entry of this record says, under Verification, that the user shell keeps the first 2048 bytes of the registry, that the list "was already cut", that the commands which fall off its end "are different ones" after the move, and that this was "not measured, and nothing checks which are shown". It did not say what the move did to what the shell shows, and no repair item recorded the truncation. Measured now on the boot logs of `3eb20421` (both images show the same):
- 41 `GKTL_COMMAND` records, 4002 bytes of registry text (each as `ours::syscall::command_list()` writes it), against a buffer that holds 2047 bytes and a NUL.
- 20 commands shown, the last one (`pagesource.test`) with its help cut; 21 not shown, `timer.test` to `tickbcast.test`, listed in the item.
- The boot listing prints "(truncated: the registry outgrew this program's buffer)" after them; the `commands` built-in, which reads the same buffer, reports every status except `NoSpace`, so it prints the same text and no tail.
- Reconstructed from `//kernel/commands`' sources list before the move, with a method that reproduces today's 20 names exactly: before it, `pager.test` was the last (half-cut) entry and `timershare.test` was not shown; after it, `timershare.test` is shown and `pager.test` is not. Not a measurement of the old build.

**Cause**
The registry is a section of records in link order, `command_list()` copies it from the start with no way to ask for the rest, and the shell's buffer is a fixed 2048 bytes because the image has no allocator. The registry has outgrown the buffer, so what a reader sees depends on where the linker put each object. Linking the four tests with x86's kernel target put them ahead of `//kernel/commands`' objects (the registration order the first entry shows), which is why the set of commands shown changed although no command did. The first entry was right that nothing reads the registry's order for lookup (`gktl::run_command()` builds a radix tree) and wrong to treat the one reader that does as a footnote: it is a user-visible list that was already half empty.

The order change is a consequence of the placement the user chose on 2026-10-30, `kernel/arch/x86/commands` over `kernel/commands/x86/`. It was not put to them as a question of its own: this entry records it and returns it as one, with the repair's options, and does not change the placement or the order to hide it.

**Change**
No code change. The repair item holds the problem for whoever takes it: enlarge the buffer, or add an offset to `command_list()` (a user ABI change, so the user's call), with a sorted list as an addition to either, and `boot_check.py` made to fail on the "(truncated" tail so a later command cannot push another off without anyone being told.

Rejected:
- Putting the four tests after `//kernel/commands` in the link to keep `timershare.test` where it was. Link order of two source sets is not a thing to tune for what a truncated list shows, and the next command anywhere would move it again.
- Fixing the buffer here. It is a different problem from where four files live, in a different library (`system/app/shell`) and, for the better option, in the system call table.

**Reference**
Zircon's `help` command, `cmd_help` (`zircon/kernel/lib/console/mod.rs:420`), prints inside the kernel and sorts by name when it may allocate; it has no buffer between the registry and the reader. The shell here is a user program reading through a system call, so the buffer is its.

**Verification**
- Registry text: `registry_size.py`, a throwaway script (not kept) that reads every `GKTL_COMMAND(NAME, CMD, HANDLER, HELP)` in `kernel/`, `benches/`, `system/` and `examples/`, joins adjacent string literals and sums `<cmd>` + (`  <help>` if any) + newline: 41 commands, 4002 bytes. The count agrees with the first entry's 41 records and "about 4000 bytes".
- What is shown: the `dbg.log` and `rel.log` of `boot_check.py` cut at the "(truncated" tail, from the first registry line (`interruptline.test`) to the cut: 2047 bytes and 20 commands in each, the same names in each. The 21 hidden are the registry minus those 20.
- The old order: the script's sources reordered as `//kernel/commands`' `sources = [...]` of `3135d44a~1` had them (mm's `dump-frame` and the two benches first, then the files in list order, the four tests among them), cut at 2047 bytes. The same method applied to today's order gives the boot log's 20 names, in order; that is its only check.
- `lint_docs.py --changed` on the repair item and on this record: no violations. No code, GN or script is touched, so no build or boot is needed for this commit; `ours-verify` runs after the last commit of this round.

**Deliberately not done**
- The buffer, the syscall and `boot_check.py` are not changed; they are the item.
- `docs/repairs/261031/shell.md` is not given a severity higher than Medium: the boot listing says it was cut, and the shell's comment accepts it. The `commands` built-in saying nothing is what keeps it from being Low.
- The first entry is not edited; this one corrects it.

**How it was found**
The independent review of `3135d44a` ran `boot_check.py --no-build` and read the end of the dbg boot log: the list opened with the four moved tests, then `dump-frame` and the benches, and ended at `pagesource.test  pagesource.test: page faults that wait for a p(truncated: ...`. For this entry the same two logs were cut at the tail and measured, the registry was summed from the sources, and `grep` over `docs/repairs` for the truncation found nothing.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Lookup of a command by name does not depend on the registry's order, so why does moving four files change anything a user can see?
- The registry is 4002 bytes and the buffer 2048. Which of the three options in the repair item would make 8 KiB the wrong answer a second time, and which would not?
- The `commands` built-in and the boot listing read the same buffer. What does each print when the registry does not fit, and why is the built-in's the worse one?
