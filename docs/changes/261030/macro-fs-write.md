# macro-fs-write changes

## Title: File.Write, and memfs the first writable filesystem, mounted at /vfs/tmp (2026-10-30)

**Files**
- `system/lib/ours.io/{include/ours_io/ours_io.hpp,src/ours_io.cpp}` — the `File.Write` method (two-way: `u32 size@16, u8 data[]@20` → Ok reply `u32 actual@20`), its codec, and the host tests for it.
- `system/lib/fsio/{include/fsio/fsio.hpp,src/fsio.cpp}` — `fsio::write()`, the client call.
- `system/service/memfs/{memfs.cpp,BUILD.gn}` — the new component: a flat pool of in-memory files served by speaking ours.io directly on a Loop; exposes `tmp`.
- `system/lib/ioservice/src/loop_server.cpp` — a forwarded Open whose write the remote's channel refuses now tells the delegate (`write_failed`), as a failed reply already did.
- `system/service/fshost/fshost.cpp` — the tree's second mount point `tmp`; the served ceiling widened to `RX_STAR_DIR | W_STAR_DIR`; a delegate that reports a failed write.
- `kernel/products/image/dev/BUILD.gn` — memfs packaged and added to root.cm; `tmp` offered from `#memfs` to `#fshost` rw*; the `vfs` offer widened with `w*`.
- `system/service/fshost/BUILD.gn` — fshost.cm uses `tmp` at /tmp rw*.
- `system/app/shell/{BUILD.gn,shell_tests.cpp}` — shell.cm uses `vfs` with `rx*` and `w*`; check_vfs creates `/vfs/tmp/hello`, writes it, reads it back through a second connection.

**Symptom**
Everything on target was read-only: a client could open and read through the composed tree, but create nothing. After this change, `shell: write(/vfs/tmp/hello) = Ok` and the bytes read back through a fresh connection — the write crossing fshost into memfs, three servers from the shell.

**Cause**
The protocol had no write method (the component-framework subset cut every mutation), and no server could create. The macro-fs plan's second milestone needs both: a writable mount to prove that the forwarding passes creation flags and write semantics through untouched.

**Change**
`File.Write` follows Read's shape with the data in the request: the seek offset is where the bytes land, the reply says how many were written. memfs is not an ioservice Tree — that library is a read-only, closed set of nodes — but a direct server over the wire and ours_io codecs, keeping the dispatch's shape: one message per wait, the Open checks in ioservice's order, an epitaph on a refused Open, a per-connection Binding on the Loop. The exposed name is the pool's root: init's forwarded Open carries `tmp`, and an Open of it binds to the root, as "." would clone it. Writing past the file's size zeroes up to the offset, as a sparse write reads back; past the file's fixed array is a short write.

The rights now narrow through three manifests: shell's use states `rx*` and `w*`, root.cm's offer carries them, fshost's served ceiling admits them, memfs's own ceiling is `RW_STAR_DIR` — and a bit missing at any layer is AccessDenied at that layer's Open.

**Reference**
Fuchsia's memfs (`src/storage/memfs`), a writable VFS with no backing device, exposed through a component's outgoing directory and mounted by fshost at `/tmp` (`main.rs:154-163`); `fuchsia.io/File.Write` for the method. The exposed-name-is-the-mount's-root rule is `OutgoingConnector::send`'s: the root opens the capability's name in the provider's outgoing directory.

**Verification**
`boot_check.py`: build ok, dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 148 (23 ours.io codec tests, the two Write ones among them). The rel log carries the whole flow: open with `maybe_create` = Ok, write = Ok, read back = Ok, `shell: vfs checks passed`. clang-tidy quiet on the touched files.

**Deliberately not done**
- No file removal, no subdirectories, no Rename/Link: the pool's rows are permanent for the process's life. A writable mount that creates and reads back is what this step proves; the rest waits for a user ask.
- No Resize: a file grows by writing past its end, up to its fixed array.
- GetBackingMemory answers NotSupported: nothing here is a VMO, and executable bytes must not come from a writable mount (design: never hand out what a revocation cannot reach).
- dbg_1 untouched (no root, no fshost, no memfs).

**How it was found**
Three failures on the way, each pinned by one line of trace (since removed):
1. `write = AccessDenied` — the shell's use stated only `rx*`: the write was refused by the use's own rights, exactly as the subset rules say. Fixed by stating `w*` too.
2. `write = PeerClosed` with the mount point dead — init's forwarded Open carries the exposed name `tmp`, which memfs treated as a file name and refused NotFound; every later forward then wrote into a closed channel. Fixed by treating the exposed name as the pool's root.
3. `write = PeerClosed` again — memfs's served ceiling lacked `GetAttributes | UpdateAttributes`, which `rw*` states, so the same Open was refused AccessDenied. The decisive line was `subset refused: have 0x1c7 want 0x1f7`. Fixed by serving with `ours_io::RW_STAR_DIR`.
The second and third failures are the same lesson twice: a mount point dies at its first refused Open, and every later use of it reads as PeerClosed.

**The user's prediction**
None (delegated, 2026-10-30).

**Three questions you should be able to answer**
- Why does memfs not use ioservice, and which parts of ioservice's shape does it keep anyway?
- Why does the exposed name `tmp` have to name the pool's root, and what breaks if the provider treats it as an ordinary path?
- Trace one write from the shell to memfs's array: which channels carry it, and which server checks which rights on the way?
