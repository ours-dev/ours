# routing-checks changes

## Title: routing_checks states the protocol its renamed Echo use speaks (2026-10-29)

**Files**
- `system/service/init/test/routing_checks/BUILD.gn` — the use of `ours.test.Echo` leaves the protocol list and becomes its own clause `{ protocol = "ours.test.Echo", type = "ours.examples.echo.Echo" }`; the comment above the target says why

**Symptom**
Latent, cannot be triggered today: nothing compares types yet. It would be triggered by the next two steps of `.agents/tasks/protocol-identity/PLAN.md`: once `route()` and the manifest compiler compare a use's type with the type of the capability its route ends at, routing_checks' use of `ours.test.Echo` ends at echo's `ours.examples.echo.Echo`, and without a stated type the two would differ, so the build (and, at run time, the first Open) would reject a route that is correct.

**Cause**
`root.cm` offers echo's capability to routing_checks `as = "ours.test.Echo"`. An `as` renames the capability, not the protocol behind it, and a type that is left out is the entry's own name. So a use that names a renamed capability has a default type, `ours.test.Echo`, that is a name nobody speaks. The one place that knows what the renamed capability really is, is the use. It is the only entry in the image whose name differs from its protocol.

**Change**
The use stops sharing a clause with the list of protocols, because a `type` goes with a single protocol:

```gn
# before
protocol = [
  "ours.process.Launcher", "ours.resource.Debug", "ours.resource.Sampling",
  "ours.resource.Irq", "ours.resource.Ioport", "ours.test.Unoffered",
  "ours.test.Echo",
]

# after
{ protocol = [ ... "ours.test.Unoffered" ] },
{
  protocol = "ours.test.Echo"
  type = "ours.examples.echo.Echo"
},
```

The use stays last, so the order of the manifest's uses, and of the entries in the component's `/svc`, is as it was. It was made in its own commit, ahead of the run-time and build-time checks, because both of those steps boot the image and each would otherwise have had to make this one-line fix, and two copies of it would conflict when they were merged.

**Reference**
None, this project's own design: `docs/design/261029/protocol-identity.md`, "Design", the default and the one place that needs a type.

**Verification**
`boot_check.py` in a clean worktree (`.worktrees/pi-format`) with this change: both images build and boot, with `routing_checks: passed`. It checks nothing about the type, since no check exists yet; it shows that the manifest still compiles and decodes, and that the uses are as many and in the same order.

**Deliberately not done**
No other manifest is changed: every other use names its capability by the name it is declared under, so the default is right. The next steps will show any that is not.

**How it was found**
By reading the manifests that rename with `as`: `root.cm` has one that renames a protocol (`ours.test.Echo`), and `boot-r` and `denied`, which rename a directory and are not compared.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does a rename with `as` need a `type` on the use, when the same manifest with no rename needs none?
2. Why is the use in its own clause and not left in the list with a `type` next to it?
3. Why is this change a commit of its own and not a part of the run-time check?
