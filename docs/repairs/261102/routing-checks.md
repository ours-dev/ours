# routing-checks outstanding repairs

## 1. boot_check cannot demand that the disk checks ran on a disked boot

**Severity**
Medium: wrong verification result, but only when a boot is run with `DISK=` set and the disk stack is broken — exactly the state the verifier exists to catch. The component now prints the signal (`routing_checks: disk checks not run (<reason>)`, 2026-11-02); nothing consumes it.

**Symptom**
Latent, cannot be triggered today on a healthy tree. With `DISK=<file>`, a broken block stack (a driver that exits, a route that dies, btreefs finding no volume) still reports `rel PASS ... 7/7 required`: the required list matches `routing_checks: passed`, which a diskless boot prints too, and the not-run line sits unread beside it.

**Cause**
`boot_check.py`'s REQUIRED mechanism is a list of lines an image must print, not conditioned on how the boot was invoked. The verifier knows whether it attached a disk (`DISK` in the environment) but does not tell the required matcher, so "this line must be absent when a disk was attached" is inexpressible. The component side of the gap is closed; the verifier side is this item.

**Change**
In `boot_check.py`, when `DISK` is set, fail the image if its log contains `disk checks not run` — the inverse condition of the required lines, keyed on the same invocation the verifier controls. The component's line stays outside REQUIRED (a diskless boot must keep passing).

Ruled out: a second required token the component prints only when the disk checks ran (`routing_checks: disk checks passed`) — it would make the component's verdict depend on the boot's shape, and `boot_check` already knows the shape better than the booted image does.

**Reference**
None, this project's own design. The shape is a test runner's skip-detection: pytest fails a `--runxfail`-style strict run when something it expected to run was skipped.

**Acceptance**
`DISK=<empty file> python3 boot_check.py --only rel` on a tree whose virtio-blk cannot start reports `rel FAIL` with the not-run line named; the same tree diskless still reports PASS. The regression this guards (40a29b42's dead block stack passing disked verification) is the case to replay.
