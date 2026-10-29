# ioservice outstanding repairs

## 1. A connection stays held in the Server when the Loop frees its slot after a failed re-arm

**Severity**
Medium: wrong, but not reached today. It needs `object_wait_async` to refuse the re-arm of a handle it accepted when the connection was added.

**Symptom**
Latent, cannot be triggered today. Were it to happen, the connection's channel is closed and its Loop slot freed, but the `Server` still holds the connection: its index is never handed out again, so one of the program's connections (128 in the root's /boot, 4 in each `Outgoing`) is lost for good, and `Server::channel()` would still answer the closed handle for it.

**Cause**
`Loop::run()` step 4 closes a slot's handle and frees the slot not only for the answer `Close` but also when step 3's `arm()` fails after an answer of `Rearm` (`system/lib/loop/src/loop.cpp`). `LoopServer::handle()` releases its connection only on the paths where it answers `Close` itself (`Server::release()` is the only way a connection is freed). Its comment in `handle()` states the invariant the other way round: "Nothing releases a connection without closing its slot in the same callback". The converse, a slot closed without the connection released, has no owner: the Loop never tells a `Waiter` that its slot was freed, and a `Waiter` cannot tell a re-arm that worked from one that did not.

It cannot happen today, since the re-arm is the wait the first `add()` armed, on the same handle with the same rights for the same signals, and the port's observer limit is checked at build time (`static_assert(LOOP_SLOTS <= ours::PORT_MAX_OBSERVERS)`). It would take a kernel that revokes a handle's rights or lowers a port's limit while the process runs.

**Change**
Not decided. The options: (a) `Waiter` gets a second virtual function, called when the Loop frees the slot for any reason but a `Close` the Waiter answered, which a `Binding` answers by releasing its connection; (b) `Loop::run()` treats a failed re-arm as an answer of `Close` to the Waiter before it frees the slot; (c) leave it, and keep the comment in `handle()` honest. (a) and (b) change the Loop's spec, which is the user's to decide, and (a) adds a second virtual function and so a larger vtable to every Waiter.

**Reference**
None, this project's own design.

**Acceptance**
A host test in `loop_server_test.cpp`, with a fake kernel that refuses the second `object_wait_async` for a handle, shows the connection released: a following `serve()` of `CONNECTIONS` connections succeeds.

## 2. Section 2.4 of the 2026-09-24 filesystem design still states the rights constants and the Open rule that the code replaced

**Severity**
Low: a document that contradicts the code. Nothing breaks, but a reader who follows section 2.4 writes a manifest or a test against rights that do not exist.

**Symptom**
`docs/design/260924/userspace-filesystem.md` section 2.4 says the rights constants "do not contain Connect", with `R_STAR_DIR = Enumerate | Traverse | ReadBytes | GetAttributes` and `SVC_DIR_RIGHTS = Connect | Traverse | Enumerate`, and says an Open whose rights exceed the node's abilities answers AccessDenied. The code differs on three points. `R_STAR_DIR` holds Connect (`system/lib/ours.io/include/ours_io/ours_io.hpp:143`). A component's `/svc` is served with `R_STAR_DIR`, not a constant of its own (`system/service/init/realm.cpp:93`). A file connection drops the rights a file has no use for instead of refusing them, and refuses only WriteBytes and an Execute the file was not added with (`system/lib/ioservice/src/server.cpp:26`, `system/lib/ioservice/src/server.cpp:308`). Sections 4.3 and 4.5 of the same document carry a note that they are superseded; section 2.4 carries none.

**Cause**
Decision D21 of `docs/design/261004/component-framework.md` (2026-10-04) chose "shrink rights instead of refusing them" and the constants as they are. The notes that mark the superseded parts of the 2026-09-24 document were added for the sections that design replaced wholesale, and section 2.4 was replaced only in part, so it was left without one. Found while writing `docs/guide/04-tree-servers.md`, which follows the code.

**Change**
Add a note in English at the head of section 2.4, in the form the document already uses (the "Revised 2026-10-03" note at its top): the constants and the third to fifth Open checks are replaced by D21 and by `ours_io.hpp` and `server.cpp`, with those paths. The Chinese text is not rewritten.

**Reference**
None, this project's own design.

**Acceptance**
Section 2.4 carries the note, and `python3 scripts/lint_docs.py --changed docs/design/260924/userspace-filesystem.md` reports nothing on the added lines.

## 3. The ioservice README names another system three times, against the rule for comments

**Severity**
Low: naming and style.

**Symptom**
`system/lib/ioservice/README.md` names Fuchsia and "the Rust vfs", with paths into that system's source tree, in its first paragraph (line 3), in the `Server` bullet (line 8) and in the `Outgoing` bullet (line 10). CLAUDE.md says code comments do not name Zircon, Fuchsia or Linux, and that where a design follows another system it says so in the change record's Reference and in the design documents, never in the code.

**Cause**
The README was written when the library was `vfs`. The renames of 2026-10-08 and 2026-10-28 kept its sentences (`docs/changes/261008/ioservice.md` says the places that name the other system's library keep their wording, since those are names in another system), and nobody asked whether a library's README counts as code for the rule.

**Change**
Not decided, and it is the user's to decide: (a) cut the three clauses from the README, since the design document and the change records already carry the reference; (b) leave them, and say in CLAUDE.md that a README may cite the system a library follows.

**Reference**
None, this project's own rule.

**Acceptance**
For (a), `grep -n "Fuchsia\|Rust vfs" system/lib/ioservice/README.md` finds nothing; for (b), CLAUDE.md and AGENTS.md say so.
