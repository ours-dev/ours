# object changes

## Title: VmaDispatcher::map() refuses a writable and executable mapping, for every caller (2026-10-03)

**Files**
- `kernel/object/vma_dispatcher.cpp` — `map()` answers InvArgs to PermWrite with PermExecute, before anything else is looked at but the option bits and the length.
- `kernel/object/include/ours/object/vma_dispatcher.hpp` — its contract says so.
- `kernel/commands/vma_wx_test.cpp`, `BUILD.gn` — new kernel test, `vmawx.test`.
- `docs/design/260923/userspace-interface.md` — an English note beside the W^X row.

**Symptom**
W^X, the system's rule that no mapping is both writable and executable (design D3), held only for programs. `vma_map` refused such a request; the kernel's own callers went straight to `VmaDispatcher::map()`, which accepted it. Until the commit before this one, the kernel's loader asked for exactly that for every program it started.

**Cause**
The objects:
- `vma_map`: the system call, which checks a program's request (W^X, Write needs Read, the rights) and then calls the dispatcher.
- `VmaDispatcher::map()`: the one place every mapping into a process is made, from `vma_map` and from the kernel's own callers (the loader for programs and the vDSO, userboot's stack).

The events:
1. W^X was written as a rule for what a program may ask, so it went into the system call.
2. The kernel's loader could not keep it, mapping images as one range, so the dispatcher could not hold anyone to it.
3. Once the loader maps by segment, every kernel caller keeps it already. Checking it in the dispatcher turns "every caller happens to keep it" into "no caller can break it".

**Change**

```cpp
if (!!(options & VmOptions::PermWrite) && !!(options & VmOptions::PermExecute)) {
    return ktl::err(Status::InvArgs);
}
```

Only the protection asked for is checked, not the ceiling: a mapping may be made read-write under a ceiling that also allows execute. `vma_protect`, when it lands, has to refuse raising a mapping to both. `vma_map` keeps its own check, which comes first in Zircon's order of errors for a program. "Write needs Read" stays a rule for programs only; every kernel caller maps readable.

**Reference**
Zircon allows a writable, executable mapping and leaves W^X to policy, by withholding ZX_RIGHT_EXECUTE. OpenBSD enforces it in the kernel for every mapping (`uvm_map()` refuses PROT_WRITE with PROT_EXEC unless the binary is marked `wxneeded`). This follows OpenBSD's choice, since D3 makes W^X the system's rule rather than a policy.

**Verification**
- The shared worktree does not build at the moment: another session's unfinished rework of the vDSO range in `kernel/mm`, not part of this commit. So the change was built and booted on an export of HEAD (975446de) with this commit's files laid over it:
  - `ninja -k 0`: no error, no warning;
  - `boot_check.py`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The shell's groups: 19 passed in dbg, 13 passed and 5 skipped in rel. All 15 probes killed, launcher checks passed. So no kernel caller asks for a writable, executable mapping.
- `vmawx.test` on dbg, through `scripts/kshell_run.py`: ok, 5 checks. Read, write and execute under a full ceiling is InvArgs; read and write, and read and execute, under the same ceiling are Ok. `vmaseal.test` still ok.
- With the check removed, `vmawx.test` fails on exactly "read, write and execute is InvArgs", so the test catches the check's absence.

**Deliberately not done**
- "Write needs Read" in the dispatcher, which the user's decision did not cover.
- `vma_protect`, which does not exist yet; the comment at the check says what it must do.

**How it was found**
The user's decision, 2026-10-03: once the kernel's loader maps by segment, hold every caller to W^X in the dispatcher.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why was this check impossible before the loader mapped programs by segment?
- Why is the ceiling not checked, and what must `vma_protect` do because of that?
- How does `vmawx.test` show something no program could show from ring 3?
