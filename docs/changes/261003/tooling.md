# tooling changes

## Title: tools/set.py checks an autorun program against the user BootFS's file list again (2026-10-03)

**Files**
- `tools/set.py` — `check_autorun()` reads the user BootFS's gen-time file list instead of the removed whitelist

**Symptom**
Since `e8eaf1be` every `tools/set.py ... --autorun <command>` failed after `gn gen` with "cannot locate the image files for the --autorun check: ERROR No targets reference the file '//kernel/products/image/dev:ours.user_visible'", and put the build directory back. A build with an autorun program could no longer be configured. Found while starting U9, which renames the programs an autorun command names.

**Cause**
The check had two inputs from `gn gen`: the image's pack response file, for the `ours.autorun.boot=` entry, and `ours.user_visible`, the whitelist of user-visible BootFS names that `kernel_package()` gathered from `bootfs_user_visible` metadata. `e8eaf1be` (userboot-bootfs step 3) deleted the whitelist with the kernel's own BootFS, and its commit message says "nothing reads it". That was checked against the build and the kernel, not against `tools/`, so the one reader outside them was missed. `gn outputs` on a label that no longer exists is an error, which the check reports as its own failure rather than as a missing program.

**Change**
The image's user BootFS is now `user_bootfs("bootfs")` (`kernel/user_bootfs.gni`), and what it carries is exactly its distribution entries. After `gn gen` the one file that lists them is the distribution manifest's unexpanded half, `bootfs.fini.dist.partial`, written by the `generated_file()` `bootfs_dist_generate`; the expanded `.dist` and `.fini` are build-time outputs and do not exist yet. A user program is a plain entry in it, so its `destination` is its BootFS name.

Before:

```python
OURS_IMAGE_WHITELIST = "//kernel/products/image/dev:ours.user_visible"
...
programs = whitelist_path.read_text().split()
```

After:

```python
OURS_IMAGE_BOOTFS = "//kernel/products/image/dev:bootfs_dist_generate"
...
entries = json.loads(bootfs_path.read_text())
programs = sorted(entry["destination"] for entry in entries if "destination" in entry)
```

The list now includes every file of the item, `vdso` and `config/init/startup` among them, not only programs. That is what the item carries, and naming one of those as an autorun command fails at boot, not here; the old whitelist excluded them only because it was built for the kernel to filter its package.

**Reference**
None, this project's own tooling. Fuchsia's `fx set` does not check `zircon.autorun.boot` against the image.

**Verification**
- `tools/set.py bringup.x86 --out out/autorun-launch --with //kernel/lib/syscall/test/fault_probe --autorun fault_probe+none` on a tree that installs programs under `bin/` (U9, in the working tree): exit 1 with "--autorun would start 'fault_probe', but the image only carries bin/fault_probe, bin/init, bin/shell, config/init/startup, vdso.", and `args.gn` restored byte for byte.
- The same with `--autorun bin/fault_probe+none`: exit 0, `args.gn` carries the command.
- Not run on a tree without U9; the check compares names and does not depend on where programs install.

**Deliberately not done**
- A unit test for `check_autorun()`: there is none for the rest of set.py either.
- Reading the expanded `.fini`: it needs a build first, and the check exists to refuse a directory before anything is built.

**How it was found**
Reading set.py for U9's autorun rename: it still named `ours.user_visible`. `gn outputs out/autorun-launch //kernel/products/image/dev:ours.user_visible` printed the error above; the earlier "autorun build passes" in `.agents/tasks/userboot-bootfs/RESULT.md` had rebuilt an already-configured directory, so set.py never ran.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the check not read the BootFS's `.fini` manifest, and what does `gn gen` write instead?
- Why did `e8eaf1be`'s "nothing reads it" miss set.py, and what search would have found it?
- What does the check now accept that the old whitelist refused, and why is that acceptable?
