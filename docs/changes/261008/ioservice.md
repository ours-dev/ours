# ioservice changes

## Title: the server library vfs is renamed ioservice, because it is the ours.io server for in-memory trees and not a mounting layer (2026-10-08)

**Files**
- `system/lib/vfs/` → `system/lib/ioservice/` — the directory, `include/vfs/` → `include/ioservice/`, the C++ namespace `vfs` → `ioservice`, the header guards, the GN targets (`source_set("ioservice")`, `test:ioservice`), the `.clang-format` include regex, and the gtest fixture names (`VfsServer` → `IoserviceServer`, and the like)
- `system/lib/ioservice/README.md` — opens by saying what the library is and is not (no mount table; a disk filesystem is a separate server)
- `system/lib/svc/`, `system/service/init/` (`BootServer`, `Realm`, the routing_checks test), `examples/echo/echo_server.cpp`, root `BUILD.gn` — the users: includes, `ioservice::` names, GN labels, and the comments that said "vfs"
- `docs/design/261004/component-framework.md` — a "Revised 2026-10-08" note, and the object table, the connection state and the test list say ioservice for ours's library
- `docs/repairs/261004/vfs.md` → `docs/repairs/261004/ioservice.md`, and `init.md`, `loop.md`, `svc.md` in the same directory — the module's repair items follow its name
- `docs/architecture/component-framework/glossary.md`, `.agents/skills/ours-kernel-design/references/boot.md` — the library is named

**Symptom**
Latent, cannot be triggered today. The name read as a mounting layer. A reader who knew Linux's VFS (a mount table in the kernel, a filesystem plugged under each mount) took `vfs` to be that, asked what mounts different filesystems into it, and concluded the library had no reason to exist. It has no mount table and mounts nothing.

**Cause**
The name was borrowed from Fuchsia's `src/storage/lib/vfs`, the library a Fuchsia server uses to serve pseudo-directories and files over fuchsia.io. ours decided on 2026-10-04 that a process has its own namespace and there is no global mount table, so the one job left to this library is answering ours.io for a tree held in memory: the connection table, the rights and path checks of Open, and the walk down the tree. "vfs" promised a layer between the client and many filesystems; the library is the server half of a protocol, whose message format is `//system/lib/ours.io`. A name should say what the thing does in this codebase's own terms, and the old one said something else.

**Change**
Renamed with `git mv`, so history follows the files, then rewritten mechanically:

```cpp
// before
#include <vfs/server.hpp>
vfs::Server m_server;

// after
#include <ioservice/server.hpp>
ioservice::Server m_server;
```

The prose that said "a vfs tree" now says "an ioservice tree", and the places that name Fuchsia's own library (`src/storage/lib/vfs/...`, "the Rust vfs") keep their wording, since those are paths and names in another system. The older change records under `docs/changes/` still say vfs and are not rewritten: they are history. `docs/repairs/261004/vfs.md` moved to `ioservice.md` because a repair file is named for its module.

Offered and not chosen: `pseudofs`, `ioserver`, and keeping the name with a clearer README. The user chose `ioservice`.

**Reference**
None, this project's own naming. The library is ours's counterpart of Fuchsia's `src/storage/lib/vfs`, which the README and the design document still cite by path.

**Verification**
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-ioservice`, at the commit before it): build ok in 7.5 s (ccache warm); dbg PASS, shell finished at 18.1 s, 35 passed, 3 skipped, 0 failed; rel PASS, shell finished at 3.9 s, 33 passed, 9 skipped, 0 failed, 6/6 required lines; host tests PASS, 143 passed, 0 failed.
- The same change built in the shared worktree failed both images with `shell: vmo_create_child checks FAILED`. That tree also held other sessions' uncommitted edits to `kernel/object/include/ours/object/vmo_dispatcher.hpp`, `system/public/ours/syscall.hpp` and `kernel/kernel/task/thread.cpp`, which touch the VMO syscall surface and were compiled into that build. A run of plain HEAD was not made, so those edits are the likely cause but are not proven to be.
- `python3 scripts/lint_docs.py --changed` on every changed document: no violations.
- `python3 scripts/format_cpp.py` on every touched C++ file and `scripts/format_gn.py` on every touched GN file: only comment re-wraps that the longer name forced.
- clang-tidy: not run, since no line changed apart from the name; this is a rename and the host tests exercise every touched translation unit.

**Deliberately not done**
- `docs/changes/` records, `.agents/tasks/component-framework/` and `.agents/tasks/vfs/`, and `docs/design/260924/`: history, kept as written.
- No behavior change, no new API: every symbol keeps its meaning under the new namespace.
- The `LoopServer` suggested by `docs/repairs/261004/svc.md` item 1 stays a repair item; the item only follows the rename.

**How it was found**
None. The user asked what vfs was for after learning that ours.io is a wire format and that nothing mounts into it.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Where does a client's `/data` come from if no library mounts it, and what does the server of that tree have to know about the path it is mounted at?
2. Which of the layers `ours.io`, `ioservice`, `fsio` fixes the byte layout of Open, which holds the connection table, and which holds the namespace?
3. Why were the "vfs" mentions that cite `src/storage/lib/vfs` left alone while the others were renamed?
