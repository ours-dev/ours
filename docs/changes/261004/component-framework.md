# component-framework changes

## Record Fuchsia's component model as the design for user space (2026-10-04)

**Files**
- `docs/design/261004/component-framework.md` — the accepted design, moved from `.agents/tasks/component-framework/DESIGN.md`, with every decision taken as recommended
- `.agents/tasks/component-framework/PLAN.md` — the 17 steps, pointing at the design's new place
- `.agents/skills/ours-kernel-design/references/boot.md` — "userboot and init" and "Filesystems and namespaces" rewritten for the root model
- `.agents/tasks/vfs/DESIGN.md`, `.agents/tasks/launcher-service/DESIGN.md` — marked superseded
- `docs/design/260924/userspace-filesystem.md` — English notes at §4.3 and §4.5 saying what is superseded

**Symptom**
None: a change of direction, not a bug. The design skill still said "Everything plugs into one Linux-style VFS", so every later step would have been reviewed against a rule the user had reversed.

**Cause**
On 2026-10-04 the user compared ours's plan with Fuchsia and chose Fuchsia's model in full. The plan of the same morning had a VFS process with a Linux-style mount table that handed out read-only VMOs, and a standalone launcher, the only VMEX holder, that resolved program names and made them executable. Research into Fuchsia showed the weak point of that split: "may execute" was a reply from the VFS that the launcher turned into a kernel right, so the launcher had to resolve names in its own view, an exception to the confused-deputy rule. In Fuchsia the Execute right on a VMO handle carries the decision itself; only a VMEX holder can add it and nobody can forge it, so the Launcher never needs to look a name up.

**Change**
The design is recorded as accepted. In the skill, before:

```text
Everything plugs into one Linux-style VFS. A filesystem library only parses,
looks up and reads; an adapter exposes it as the VFS's file-operation set.
```

After:

```text
Follow Fuchsia, not Linux (decided 2026-10-04, reversing the Linux-style VFS
of 2026-09-26). There is no VFS process and no global mount table: each
process has its own namespace, a client-side table from path prefix to
directory channel (fdio's `fdio_ns_t`), filled from its startup handles.
```

Rejected: keeping the split (a VFS process plus the launcher), and merging only /boot into the launcher; both are in the design's "Alternatives considered".

**Reference**
Fuchsia's component_manager (`src/sys/component_manager/src/builtin_environment.rs`), BootfsSvc (`src/sys/component_manager/src/bootfs.rs`), the fdio namespace (`sdk/lib/fdio/namespace/local-filesystem.cc`) and `fuchsia.process.Launcher` (`sdk/fidl/fuchsia.process/launcher.fidl`).

**Verification**
`python3 scripts/lint_docs.py --changed` on every document touched: no violations. No code changed, so the build and boot are unaffected.

**Deliberately not done**
No code: the steps land one commit each, starting with step 2. The old Chinese design text is not rewritten; it gets English notes only.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a launcher that resolves names need an exception to the confused-deputy rule, and why does Fuchsia's Launcher not?
- What plays the role of Linux's mount table in Fuchsia, and who fills it?
- Why can a VMO handle with the Execute right not be forged by a client that serves its own directory?
