# build changes

## Title: user programs install under bin/, and code takes their names from a generated header (2026-10-03)

**Files**
- `system/user_executable.gni` — `user_program_dir = "bin"`; `install_name` defaults to `bin/<target>`; a `<target>.install_name` group carries the name as `user_program_names` metadata
- `system/user_program_names.gni`, `system/user_program_names.py` (new) — `user_program_names()`: a header of `constexpr std::string_view` names in namespace `user_programs`, written from that metadata
- `kernel/lib/userabi/userboot/BUILD.gn` — `install_name = "userboot"`, pinned at the top level
- `system/service/init/init_startup.gni` — an entry's default name follows the same rule, `bin/<target>`
- `system/service/init/BUILD.gn`, `init.cpp` — `user_program_names("programs")` for the shell; `SHELL{"shell"}` goes
- `examples/shell/BUILD.gn`, `shell_tests.cpp` — the same for fault_probe; the literal `"fault_probe"` goes
- `examples/shell/shell.cpp` — `command_path()`: a command without `/` is looked up in `bin/`
- `kernel/products/image/dev/BUILD.gn`, `dev.dbg_1/BUILD.gn` — `userboot.next=bin/init` and `userboot.next=bin/shell`
- `kernel/user_bootfs.gni`, `build/product/ours_args.gni`, `tools/set.py`, `.agents/skills/ours-kernel-design/references/boot.md` — say where programs land and that an autorun command names the whole path

**Symptom**
None, a planned change: U9 of `docs/design/260924/userspace-filesystem.md`. Before it, every user program sat at the top level of the user BootFS next to `vdso` and `config/`, and the names programs were started by were string literals in four places: both cmdlines, `init.cpp`'s `SHELL{"shell"}` and the shell's `"fault_probe"`.

**Cause**
Two things were wrong with the literals, and the second is the one worth remembering.

The objects:
- A program's install name: the BootFS path `user_executable()` packages it under. It is decided in GN.
- A starter: code that names a program to start it (init names the shell, the shell names fault_probe).
- The launcher's lookup: `bootfs::View::find(name)`, an exact match on the path.

The events, before:
1. GN installs fault_probe as `fault_probe`.
2. The shell's source says `"fault_probe"`, typed by hand.
3. Someone renames the program or moves it, here into `bin/`.
4. Everything still builds. At boot `find("fault_probe")` misses, and the probe run reports NotFound.

The name exists twice, in two languages, and nothing compares them, so a mismatch shows only at boot.

The obvious fix fails. Generating the header with `generated_file()` walking the programs' own metadata needs the header target to depend on the programs. A dependency on a program is not only an ordering edge:
- the user BootFS packages every file in its deps' metadata, so the dev image, which carries the shell but not fault_probe, would get fault_probe through the shell's header;
- two programs that start each other would be a dependency cycle.

**Change**
Each `user_executable()` now defines a second target, `<name>.install_name`, a group with no deps whose only content is metadata:

```gn
_program_name = target_name
group("$target_name.install_name") {
  metadata = {
    user_program_names = [
      {
        name = _program_name
        path = _install_name
        label = get_label_info(":$_program_name", "label_no_toolchain")
      },
    ]
  }
}
```

`user_program_names()` depends on those groups, not on the programs. Its `generated_file()` collects the metadata as JSON at gen time, and the Python action writes the header:

```cpp
namespace user_programs {
/// //examples/shell:shell
inline constexpr std::string_view SHELL{"bin/shell"};
/// Where a program installs unless it names a place of its own.
inline constexpr std::string_view DIRECTORY{"bin"};
} // namespace user_programs
```

Before and after in init:

```cpp
constexpr std::string_view SHELL{"shell"};
...
return start_then_write(launcher, job, SHELL, ...);
```

```cpp
#include "programs.hpp"
...
return start_then_write(launcher, job, user_programs::SHELL, ...);
```

A program that is renamed renames its constant, and one that is dropped is a compile error in whoever still starts it. The generator refuses two programs whose constants collide, and a program named `directory`.

Other decisions:
- **userboot stays at the top level.** The kernel finds it by `kernel.select.userboot`, default `"userboot"`, in the kernel package, as Zircon's kernel finds a bare `userboot` in its STORAGE_KERNEL BootFS.
- **The shell's lookup is a PATH of one entry.** A command with a `/` is taken as typed; any other becomes `bin/<command>`, with `user_programs::DIRECTORY` as the directory. Nothing else is searched: the other top-level files are the vDSO and configuration, and a fallback would only turn a typo into an attempt to run one of them. argv[0] stays as typed, as a Unix shell leaves it.
- **An autorun command names the whole path**, `bin/fault_probe+none`, as Zircon's `zircon.autorun.boot` takes `/boot/bin/...`. init applies no lookup.

Two GN rules this ran into:
- A template sees only the variables its file had defined *before* the template. `user_program_dir` defined after it was "not currently in scope" at every call.
- Inside a target block nested in a template, `target_name` is that block's own name. The first header said `FAULT_PROBE_INSTALL_NAME`, because the group's `target_name` is `fault_probe.install_name`; the program's name is now saved before the group.

**Reference**
- Zircon installs programs under `bin/` in its BootFS; userboot's default `userboot.next` already carries the directory (`zircon/kernel/lib/userabi/userboot/main.rs`, `parse_option()`).
- A PATH lookup: `execvp()` in libc, and dash's `find_command()` in the shell Fuchsia ships.
- The generated header and its metadata-only targets have no Zircon counterpart: Zircon's starters spell paths such as `/boot/bin/sh` by hand. This project's own design, to make a renamed program a compile error.

**Verification**
- `boot_check.py --out out/verify-skill`: dbg PASS (20 passed, 16 probes killed with -1028), rel PASS (18 passed).
- rel log: `userabi: passing userboot.next=bin/init to userboot`, `userboot: started bin/init`, `init: launched shell`, `init: shell says "pong"`. dbg log: `userboot: started bin/shell`.
- The kernel package still lists `userboot` at the top level; the user BootFS is `bin/fault_probe`, `bin/init`, `bin/shell`, `config/init/startup`, `vdso` (the shell's `ls`), and the startup list reads `bin/shell init`.
- `out/autorun-launch` (rel, `--autorun bin/fault_probe+none`): PASS, with `init: autorun bin/fault_probe+none` and fault_probe's own lines.
- Typed into the rel shell over serial: `fault_probe none` and `bin/fault_probe none` both run fault_probe; `vdso` and `nosuch` answer "no such program in the BootFS: bin/vdso" and "bin/nosuch".
- The generator, by hand: a program named `directory` and two programs that both make `A_B` each exit 1 with a message.

**Deliberately not done**
- A default for `userboot.next`. The design row says to change the default to `bin/init`, but userboot has none: without the option it says hello and halts, and both images set it.
- Process names keep the directory: the kernel logs `process 'bin/fault_probe'`. The launcher names a process by the BootFS name it was given. Whether to name it by the last component instead is left for the launcher service.
- init reads no startup list yet: it starts the shell from `user_programs::SHELL`, and `config/init/startup` is only packaged.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the header target depend on `fault_probe.install_name` rather than on `fault_probe`, and what would go wrong in the dev image if it depended on the program?
- Why does `bin/vdso` fail to run from the shell, and why is there no fallback to the top level?
- Why must `user_program_dir` be defined above `template("user_executable")` in its file?
