# abi changes

## Title: Every text that describes the ABI says what the code does (system-abi-surface step 1) (2026-10-02)

**Files**
- `system/public/ours/syscalls.gni` — the doc strings of most calls, the `release` definition and one `release` flag (vmo_replace_as_executable's |vmo|), and the header comment.
- `system/public/ours/syscall.hpp`, `status.hpp`, `signals.hpp`, `rights.hpp`, `vdso.hpp` — the public headers' comments, two of them attached to the wrong enumerator.
- `system/lib/ou/include/ou/*.hpp`, `system/lib/ou/README.md`, `system/lib/ou/BUILD.gn`, `build/abi/gen_syscalls.py` — ou's restated contracts and the generator's docstring.
- `kernel/lib/syscall/src/*.cpp`, `kernel/lib/syscall/BUILD.gn`, `kernel/object/...`, `kernel/lib/userabi/...`, `kernel/arch/x86/kernel/syscall.S`, `kernel/lib/syscall/test/fault_probe/trap.S` — comments about the calls, the handle table, the vDSO check and the loader.
- `docs/design/260923/userspace-interface.md` — English notes on rows that no longer hold, and a "Corrected 2026-10-02" block under the Zircon-deviation table.
- `.agents/skills/ours-kernel-design/references/layering.md`, `system/user_executable.gni`, `system/BUILD.gn`, `examples/shell/BUILD.gn`, `examples/shell/shell.cpp` — who binds a program to the vDSO, and how; and that the shell's ABI checks still call `ours_*` directly.
- `.agents/tasks/system-abi-surface/DESIGN.md` — step 1 marked done.

**Symptom**
The texts a program's author reads said things the code does not do. Some examples:
- command_list was "always NUL terminated"; a buffer the kernel cannot write gets InvArgs and may be left part-written with no terminator.
- object_wait_one's doc read as if any deadline worked; anything but 0 and INSTANT_INFINITE is NotSupported.
- vmo_replace_as_executable consumes its handle whatever the answer, but its argument had no `release = true`.
- process_spawn_boot said nothing can wait on a process; a process handle from process_create can be waited on for Terminated.
- NoSpace was "about a filesystem or device"; its one system-call use is command_list's too-small buffer.
- status.hpp's "maximum number of status values" comment sat on PeerClosed, and BadPath carried NotDir's comment.
- signals.hpp named an event's SIGNALED and a job's or thread's TERMINATED; there is no event object, and only a process raises Terminated.
- Several texts still had the kernel mapping the vDSO and `bind_imports()` binding programs, two designs ago, and described the vDSO check as one on the return address.

**Cause**
The objects:
- One fact, stated in many places: the call's description in `syscalls.gni` (rendered into the declaration a program sees), ou's header that restates it by hand, the kernel handler's comment, the design document, and the skills.
- The code, which is the only one of them anything tests.

The events:
1. A text is written as a plan ("after K23", "P1"), or as a description of the code at the time.
2. The code changes in a later commit: the vDSO moved out of `ProcessDispatcher::create`, `bind_imports()` became `elfload::Binder`, K23 landed.
3. The commit updates the copy next to the code it touched, if any, and the other copies drift.

Nothing failed, because nothing reads a comment. That is why the drift was found by reading, not by a build: the expert panel's review of the ABI surface, then this sweep.

**Change**
How the record was made true:
1. An audit, one agent per call family (debug, handle-object, channel, task, memory) plus one for the texts about the ABI as a whole. Each reported the file, the current text, what the code does with `file:line` evidence, and a replacement.
2. A skeptic per family re-checked every finding against the code: 106 found, 92 held, 13 held in part with a corrected text, 1 refuted (raising CHANNEL_MAX is compatible for programs already built). The skeptics reported 20 more, which a second pass verified: 13 held, 7 duplicated the main list.
3. The verified texts were applied, a script for exact matches, by hand or by an agent for the rest, and the applied diff was reviewed again, file group by file group, before commit.

Two examples:

```text
// syscalls.gni, command_list -- before
One command per line, always NUL terminated.
// after
One command per line, NUL terminated when the answer is Ok or NoSpace. ...
A null |buffer| or a |length| of 0 is InvArgs, and so is a buffer the kernel
cannot write, which may be left part-written and unterminated.
```

```text
// vdso.hpp -- before
the kernel refuses a trap whose return address lies outside it
// after
A process whose `syscall` instruction does not lie wholly inside the range its
vDSO mapping registered is killed (kernel/lib/syscall/src/table.cpp,
trapped_from_vdso()).
```

Old Chinese documents were not rewritten: each correction is an English note beside the text it corrects, as CLAUDE.md asks.

**Reference**
Zircon keeps each call's contract in one place, its FIDL description (`zircon/vdso/*.fidl`, for example `vmar.fidl`), which carries the doc comments and from which the declarations are generated. Here `syscalls.gni` plays that part for the declarations, but ou restates every contract by hand. That second copy is what steps 8 and 9 of the system-abi-surface design remove, by generating ou from the same description.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The generated declarations (`vdso_decls.inc`) rebuild from the changed descriptions; `release` is validated by `gen_syscalls.py` and written into no output, so the flags change no generated code.
- `format_cpp.py` clean on every changed C/C++ file, `format_gn.py` clean on every changed GN file, `lint_docs.py` clean on the documents.
- The applied diff was reviewed again in four file groups, each re-checked by a skeptic. It found no code change and no false statement left in the audited texts, but found the application's own faults, all fixed before commit:
  - Chinese text rewritten in place in the ou README and in one design-document row, now restored with the English beside it;
  - three files still giving the pre-K23 answer to who maps and binds the vDSO (`system/BUILD.gn`, `examples/shell/BUILD.gn`, `userboot.hpp`);
  - a handful of new statements that went too far: "never traps directly", "never unmapped", the name buffers, the out-parameter and error-order notes;
  - one rewrap left ragged in the generated declarations.
- Applying the texts had its own lesson: a script that replaced quoted text with proposed text took four editing instructions ("Add under the table", "Delete line 98") for text, and turned four table rows into list items. Each was caught by formatting, linting or reading the diff, and redone by hand.

**Deliberately not done**
- Behaviour. Step 1 changes text only, so what the audit found questionable in the code itself stays as it is and is said as it is: command_list answers NoSpace, where BufferTooSmall would match the rest of the ABI, and channel_create writes |out0| before it can fail on |out1|. Which status codes the ABI should use is the open question in the design.
- The 18 direct `ours_*` calls in `examples/shell` are now described, not removed (step 5).

**How it was found**
The expert-panel review of `//system`'s ABI surface (`.agents/tasks/system-abi-surface/DESIGN.md`, step 1), run at 06:49 and again on the tree after K23 and CanMapSpecific had landed, since its first findings had gone stale.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did none of these false statements fail a build or a test?
- Which copy of a call's contract does a program's author see, and which ones can drift from it?
- What will generating ou from `syscalls.gni` remove, and what will it not?
