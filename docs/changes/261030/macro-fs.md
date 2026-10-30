# macro-fs changes

## Title: the composing server fshost mounts /boot, and the shell reads through two servers (2026-10-30)

**Files**
- `system/service/fshost/{fshost.cpp,BUILD.gn}` — the new component: takes the routed `boot` NsDir, adds it to its tree as a remote entry under `vfs`, serves the tree on its DirectoryRequest; exposes `vfs`.
- `kernel/products/image/dev/BUILD.gn` — fshost packaged and added to root.cm between echo and shell; the built-in `boot` offered to it; `vfs` offered from `#fshost` to routing_checks and the shell.
- `system/app/shell/{BUILD.gn,shell_tests.cpp}` — shell.cm uses `vfs` at /vfs rx*; `check_vfs()` lists /vfs, then opens and reads `/vfs/boot/bin/shell`.
- `system/service/init/test/routing_checks/{BUILD.gn,routing_checks.cpp}` — a fifth directory use `/vfs`, with a read through it and listings of /vfs and /vfs/boot.

**Symptom**
Before this change the forwarding machinery ran nowhere: no component provided a directory, so no Open ever crossed servers. After it, a read of `/vfs/boot/bin/shell` walks two servers — fshost's tree, then the root's BootFS server — and returns the ELF header.

**Cause**
The last piece the macro-filesystem plan needed: a real provider. Without one, step 1's forwarding and step 2a's routing were only tested code paths, and a composite tree could not exist to be handed to a user.

**Change**
fshost's whole main is three objects and no protocol code: it takes the one NsDir named /boot (matched as fsio matches, by name index), `Tree::add_remote(vfs, "boot", channel)` makes the mount point, and `LoopServer::serve(directory_request, root, RX_STAR_DIR)` answers Opens. An Open of `vfs` from init binds the requester to the `vfs` node; an Open below it reaches the remote entry and is forwarded with the remainder — the per-open state is the requester's own server end, carried in the Open itself.

Verified on dev (rel image): `init: started fshost`, then `shell: open(/vfs/boot/bin/shell) = Ok`, `read = Ok` with the `\x7fELF` magic, and routing_checks' three new checks green (`routing_checks: passed`, 6/6 required). dbg (dev.dbg_1, no root) skips the checks, as its other namespace checks do.

**Reference**
Fuchsia's fshost export pseudo-directory, whose entries are `remote_dir(...)` mounts (`src/storage/fshost/src/main.rs:154-163`); ours's fshost is that pseudo-directory with a component manifest around it.

**Verification**
`boot_check.py`: build ok, dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 148. clang-tidy quiet on fshost.cpp.

**Deliberately not done**
- No `tmp` mount yet: memfs is the next change (plan step 3); the tree's NODES=8 leaves room.
- No interactive shell command over /vfs: the checks cover it; `ls /vfs` in the command shell waits for a user ask.
- dbg_1 is untouched: it has no root, so no fshost and no /vfs there.

**How it was found**
Two failures on the way, both from the boot logs:
1. `routing_checks: /vfs = BadPath` — the Open init forwarded carried the path `"vfs/"`: the '/' was joined unconditionally and the subdir was empty, and a trailing slash is a BadPath. Fixed by joining the '/' only when a subdir follows (`docs/changes/261030/macro-fs.md`, the fix entry below).
2. `shell: open(/vfs) = NotFound` — root.cm offered `vfs` to the shell, but shell.cm did not use it, so no NsDir was bound at /vfs. An offer without a use hands nothing; the shell check then hangs were an empty-bodied `for` in check_vfs, fixed into the same shape routing_checks' check_read_dirents has.

**The user's prediction**
None (delegated, 2026-10-30).

**Three questions you should be able to answer**
- Why does fshost need no protocol handler of its own, and what makes an Open of a path below `boot` leave fshost for good?
- Why must an offer in root.cm be paired with a use in the child's manifest, and what does the child see when only one side is written?
- Why is a trailing '/' in an Open's path refused, and which layer refuses it?

## Title: the Open a directory route forwards carries no trailing slash (2026-10-30)

**Files**
- `system/service/init/realm.cpp` — the name and subdir of a provider-routed directory are joined with a '/' only when the subdir is not empty.

**Symptom**
The first component-provided directory use on target (fshost's `vfs`, same day) answered every open under it with BadPath: `routing_checks: /vfs = BadPath (expected Ok) FAILED`.

**Cause**
The join wrote `name + '/' + subdir` unconditionally. With an empty subdir the path was `"vfs/"`, and a trailing '/' is not a valid ours.io path — `is_valid_path` refuses it (a path is either "." or names joined by '/', with no empty segment), so fshost's server answered the Open's epitaph BadPath and the client's namespace entry was closed with it. Every later open under the entry answers the recorded epitaph without touching the channel (fsio's `mark_closed`), which is why the failure repeated exactly.

**Change**
```cpp
// before
path_bytes[routed->name.size()] = '/';
std::memcpy(... + routed->name.size() + 1, subdir.data(), subdir.size());
forward(..., {path_bytes.data(), routed->name.size() + 1 + subdir.size()}, ...);
// after
if (!subdir.empty()) {
    path_bytes[routed->name.size()] = '/';
    std::memcpy(... + routed->name.size() + 1, subdir.data(), subdir.size());
}
forward(..., {path_bytes.data(), joined}, ...);   // joined = name + subdir + (subdir.empty() ? 0 : 1)
```

**Reference**
`fs::Traverse`'s path rules (`src/storage/lib/vfs/cpp/vfs.cc:52-64`): a path is "." or names separated by '/'; fuchsia.io's Path invariants forbid a trailing '/' (`io.fidl:40`), and ours.io keeps them (design D21).

**Verification**
`boot_check.py` rel PASS after the fix, with routing_checks' `/vfs`, `/vfs/boot` and the two-hop read all green.

**Deliberately not done**
None.

**How it was found**
From the rel boot log: every open under /vfs answered BadPath while /boot worked, and the only difference between the two entries was which server ended the route — pointing at the Open init wrote, not at the server that answered it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did the client keep answering BadPath for every later open, instead of hanging or returning something else?
- Where is a trailing slash meaningful in a POSIX open, and why does ours.io refuse it instead?
- Why is the empty-subdir case the common one for a directory capability, unlike a protocol's always-non-empty `svc/<name>`?
