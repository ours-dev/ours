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
