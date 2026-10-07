# build changes

## User code keeps a frame pointer in leaf functions (2026-10-07)

**Files**
- `build/config/BUILD.gn` — `default_frame_pointers` picks the new `frame_pointers_in_leaves` (`-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`) in the user toolchain; the kernel and phys keep `frame_pointers`
- `build/toolchain/ours/elf_link.py` — the new flag is a compiler-driver flag, kept off the linker's command line
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `RESULT.md`, `.agents/skills/ours-kernel-design/references/task.md` — the decision

**Symptom**
Every user sample taken in a leaf function lost that function's caller: profile's stacks read `spin_inner` → `main`, with `user_work` missing.

**Cause**
User code was built with `-momit-leaf-frame-pointer`, so a leaf did not push a frame. The sampler walks user stacks by frame pointers, and in a leaf rbp still points at the caller's frame, so the first return address it reads is the caller's own, into the caller's caller.

**Change**
User code keeps a frame in leaf functions too; it costs a leaf call its `push %rbp; mov %rsp,%rbp` and the `pop`. The user weighed a build switch that keeps the frames only in a profiling build, and chose to keep them always, so that any build can be profiled as it is. A switch at run time was not an option: whether a function builds a frame is fixed when it is compiled. The kernel keeps `-momit-leaf-frame-pointer`, since its stacks are unwound through `.eh_frame`, which describes every instruction.

What still hides a caller is a function's first instructions, before its frame is pushed, and its last, after the pop: a frame-pointer walk cannot see a frame that is not there yet or any more.

**Reference**
None in the upstream kernel for this choice. Distributions that profile with frame pointers have made the same one; not checked here.

**Verification**
`spin_inner` in `bin/profile` now starts `push %rbp; mov %rsp,%rbp`; a kernel target's compile still carries `-momit-leaf-frame-pointer`. In a clean worktree at HEAD with only this change: a full build, and `boot_check.py`: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required), `profile: 43 kernel samples, 8 user samples, 0 dropped`; 7 of the 8 user samples read `spin_inner` → `user_work` → `main` → `ort_start` → `_start`.

**Deliberately not done**
None.

**How it was found**
The user asked whether to keep leaf frames, and chose always.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a sample in a leaf without a frame lose the leaf's caller and not the leaf itself?
- Why could the frames not be switched on only while a session runs?
- Why does the kernel not need them?
