# shell changes

## read_vdso_layout() parses the vDSO's whole file, not its first page (2026-10-12)

**Files**
- `system/app/shell/shell_tests.cpp` — `read_vdso_layout()`: `vmo_get_size()`, then the whole VMO read into a 64 KiB static buffer and parsed.

**Symptom**
Latent until the vDSO grew: with `object_get_child` added (the job-processes step of `.agents/tasks/process-introspection/PLAN.md`), both images failed `shell: vdso map checks FAILED (its code not found)` and `shell: vma_unmap checks FAILED`, though `vmo_read(vdso, its headers) = Ok`.

**Cause**
`read_vdso_layout()` read the vDSO VMO's first page and handed it to `launch::Program::read()` as if it were the whole file, on the reasoning that the headers lie in the first page. They do, but the ELF parser checks every segment's file range against the bytes it is given, and the vDSO's code segment had grown to cross the first page in the file: `LOAD 0x000d30 ... FileSiz 0x0002e8 R E` (`llvm-readelf -l` of `vdso.image.elf`) ends at file offset 0x1018. The parse failed, the layout came back empty, and both checks that use it reported the vDSO's code missing. The kernel's own read of the vDSO was unaffected: it reads the file whole.

**Change**
The function reads the VMO's size and then the whole VMO, which is the vDSO's file, and parses that. A static buffer of 64 KiB, several times the vDSO's 12 KiB, since the shell runs its checks on one thread.

**Reference**
None, this project's own test.

**Verification**
In `.worktrees/wip-proc-self` at baseline `007a72c1` plus only this change: dbg PASS (37 passed), rel PASS (35 passed, 6/6 required), host tests 144 passed. With the job-processes step's `object_get_child` on top, which grows the vDSO, both checks pass with this change and fail without it.

**Deliberately not done**
None.

**How it was found**
The job-processes step's first boot: the vDSO checks failed with `vmo_read` Ok; `llvm-readelf -l` on the new `vdso.image.elf` showed the code segment's file range past the first page.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did reading only the page with the headers work until the vDSO grew?
2. Why did the kernel, which maps the same vDSO, not fail too?
3. What does an ELF parser check a segment's file range against, and why?
