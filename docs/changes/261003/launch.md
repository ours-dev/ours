# launch changes

## Title: The BootFS launcher moves from the shell into the launch library (2026-10-03)

**Files**
- `system/lib/launch/include/launch/bootfs.hpp` (new) — `BootfsLauncher`, `make_bootfs_launcher()` and `launch_from_bootfs()`.
- `system/lib/launch/src/bootfs.cpp` (new) — their definitions.
- `system/lib/launch/BUILD.gn` — the new source and header, and `//system/lib/bootfs:headers`.
- `examples/shell/shell.cpp` — the shell now calls the library:
  - `make_launcher()` and `launch_program()` are thin wrappers;
  - `find_in_bootfs()` and the 16 KiB `vdso_bytes` copy are gone;
  - `list_bootfs()` reads the mapped BootFS through `bootfs::View`.
- `examples/shell/shell_launcher.hpp` — `shell::Launcher` is now `launch::BootfsLauncher`.

**Symptom**
None: a refactor, the first half of step U7 in `docs/design/260924/userspace-filesystem.md`. init is to start the shell with the launcher, and the code that prepares a launcher lived only in the shell, about 160 lines of it.

**Cause**
The objects:
- `launch::start()`: starts one program. It needs:
  - the program's file as an executable VMO;
  - the same bytes readable in this process;
  - the vDSO's VMO and the vDSO's file.
- The BootFS VMO, which holds every program.
- vmex, the resource that lets a VMO be made executable.

The events, once per launcher:
1. Read the BootFS header.
2. Map the whole BootFS read-only.
3. Find `vdso` in it.
4. Make an executable copy of the BootFS with vmex.

The events, once per program:
1. Find the program by name.
2. Slice its file VMO out of the executable copy.
3. Call `start()`.

None of this is the shell's own business, and init needs the same.

The shell's copy also had two limits. A `bootfs::View` over the mapping removes both:
- It parsed the directory through `vmo_read` into fixed buffers, at most 16 entries and 512 bytes of names. View reads the directory in place and checks every entry's extent.
- It copied the vDSO into a 16 KiB static buffer, though the vDSO already lies in the mapping. `vdso_file` is now a span into the mapping.

**Change**

```cpp
// before, in the shell: ~80 lines of vmo_read, vma_map and vmex
auto make_launcher(...) -> Launcher { ... find_in_bootfs(bootfs, "vdso", &entry) ...
                                      ours::vmo_read(bootfs, vdso_bytes, ...) ... }

// after: the library, reporting each failing step through elfload::Diagnostics
auto made = launch::make_bootfs_launcher(bootfs, vmex, vdso, vma,
                                         {.report = print_launch_report, .context = nullptr});
...
return launch::launch_from_bootfs(launcher, job, name, bootstrap,
                                  {.report = print_launch_report, .context = nullptr});
```

Failures go through the same `Diagnostics` that `start()` already uses, as an Error whose `what` names the step. The caller adds its own prefix, so the library prints nothing itself.

`BootfsLauncher` stays a plain struct of handles and spans, like the library's `Request` and `Started`. `launch_from_bootfs()` therefore re-runs `View::from()` on each launch instead of trusting a stored view; that costs one pass over the directory. The caller owns `executable` and closes it.

Rejected:
- Copying the code into init. Two copies would drift apart. This was the user's choice between the two.

**Reference**
- Fuchsia's component_manager parses the BootFS once and hands each program out as its own VMO, made executable with vmex: `src/sys/component_manager/src/bootfs.rs`, `create_vmo_from_bootfs()`.
- A launcher then gets a program's VMO by path before process_builder starts it.

**Verification**
- `ninja -k 0` on `out/bringup.x86`: no error or warning.
- `boot_check.py`:
  - dbg PASS, 20 passed: 16 probes killed, launcher and object_get_info checks passed;
  - rel PASS, 18 passed.
- The interactive path, typed over the rel image's serial port with a one-off script built on `kshell_run.Console`:
  - `ls` lists the five BootFS entries, through View;
  - `help` answers;
  - `shell` launches a child shell, which runs its checks and ends, and the prompt comes back;
  - `nosuch` reports `shell: launch failed: no such program in the BootFS: nosuch`.

**Deliberately not done**
- init does not use it yet. That is the second half of U7.
- `describe_bootfs()` and the self-checks in `shell_tests.cpp` still read the BootFS through `vmo_read`. They check that system call on purpose.

**How it was found**
None: a planned refactor.

**The user's prediction**
None. The user chose a shared library over a copy in init.

**Three questions you should be able to answer**
- Which of the launcher's three parts does `launch::start()` need, and why each one?
- Why does `make_bootfs_launcher()` read the header through `vmo_read` before it maps anything?
- What two limits did the shell's copy have that a `bootfs::View` over the mapping removes?
