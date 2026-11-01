# fshost-admin changes

## Title: the tree grows at run time: fshost's Admin.Mount (2026-11-01)

**Files**
- `system/lib/ours.fshost/` — the new admin codec: `Admin.Mount`, a two-way
  call whose request carries a directory handle and a name; four host tests.
- `system/service/fshost/` — an `svc/ours.fshost.Admin` service node in the
  tree, a one-connection admin server in the delegate, and the Mount
  handler: validate the name, copy it into owned storage, `add_remote` it
  under the writable view. The manifest exposes the protocol; the node
  budget grew to 16 for the mounts to come.
- `kernel/products/image/dev/BUILD.gn` — the Admin protocol and a writable
  view (`vfs-all`) routed to routing_checks, whose `/vfs` stays the
  read-only view on purpose.
- `system/service/init/test/routing_checks/` — check_disk mounts the disk
  at run time under "extra", creates and reads a file through the mounted
  path, reads the same bytes through `/disk`, unlinks via `/disk`, and
  proves the mount stays out of the read-only view.

**Symptom**
None — the parked macro-fs decision ("dynamic mount shape: config vs admin
protocol"), unblocked now that block devices exist. Until now a mount point
existed only if root.cm offered it before the system started.

**Cause**
The composing server's tree is a preallocated node array whose remote
entries hold directory capabilities; mounting at run time is one verb:
hand the server a capability and a name, and `Tree::add_remote` grows the
writable view. The protocol is the capability boundary — whoever can reach
`ours.fshost.Admin` can reshape `/vfs` — so the protocol routes only to the
check component, exactly like the resource roots.

**Change**
`Mount(handle, name)`: the name must be one component (no separators, no
dot forms) and is **copied into fshost's own storage before `add_remote`**
— the tree keeps names as views, and the request's bytes are the shared
message buffer the next read overwrites; this was the defect the first
run's NotSupported traced to, via fshost's `open_refused` delegate hook.
The mounted directory serves through the mounted path immediately; opens
below it forward to the mounted filesystem exactly as the config-time
mounts do. AlreadyExists keeps the second Mount honest (fshost is resident
and its tree outlives a check run). Unmount is deliberately absent: taking
a mount point down wants a rule for the connections walking it.

**Reference**
`fuchsia.sysmates`/... the closest analogue is Component Manager's
`fuchsia.component.Realm` + `RealmPatcher` world pared to one verb; the
capability-handoff shape (client opens a directory, passes the handle in a
call, the server adopts it) is `fuchsia.io`'s own
`Directory.Open`-with-object, one level up. The view isolation (mounts
appear in `all`, never in `ro`) is the macro-fs design's structure-is-
policy rule holding without new code.

**Verification**
Host: 4 ours.fshost codec tests (round trip, empty/oversized names, the
255-byte cap boundary, missing handle, the status-form reply);
`boot_check.py` dbg PASS (44), rel PASS (42, 6/6 required), host-tests
PASS 158 -- diskless, where the mount check skips. With the GPT disk:
`routing_checks: passed` -- the mount lands, a file created through
`/vfs-all/extra/` reads back through a fresh open AND through `/disk/`
(one filesystem, two namespace names), the unlink goes through `/disk`
(the tree server does not forward Unlink yet), and `/vfs/extra` opens Ok
but reads NotFound -- the read-only view's epitaph surfacing at first use.
clang-tidy quiet.

**Deliberately not done**
- Unmount: wants a rule for active connections; the mount point dies with
  fshost for now.
- Unlink/ReadDirents *through* a mount point: the tree server forwards
  Open only; a removal or listing aimed at a mounted path must go via the
  mounted filesystem's own name. Exposed by this check and deferred.
- Persistence: mounts live in fshost's memory; a boot re-mounts from the
  config, and a runtime-mount config file is future work.

**How it was found**
The first run's NotSupported was pinned by fshost's `open_refused` hook:
`path "extra/mounted-check.txt" status NotSupported` -- the tree's
creation-gate answer for a name it could no longer see, because the node's
name view pointed at the shared message buffer the next request
overwrote. The tree's own doc comment says the bytes must outlive it;
fshost was the first caller to violate that from a message.

**The user's prediction**
None. The user approved the slice and the single-level/`all`-view scope.

**Three questions you should be able to answer**
- Why must fshost copy the mounted name before `add_remote`, and which
  earlier commit's design decision (D20-adjacent) made the tree take a
  view at all?
- The read-only view's probe opens "extra" and gets Ok -- what does the
  handle point at, and at which exact call does NotFound appear?
- Who may call Admin.Mount in this image, and what in the routing makes
  that a capability decision rather than a code check?
