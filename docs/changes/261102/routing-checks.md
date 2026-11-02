# routing-checks changes

## Title: the verdict names what did not run: a pass with the disk suite skipped says so (2026-11-02)

**Files**
- `system/service/init/test/routing_checks/routing_checks.cpp` — `BLOCK_DEVICE_SEEN` and `DISK_CHECKS_RAN`, set where the block probe answers and where the mounted filesystem is first reached; the verdict prints `routing_checks: disk checks not run (<reason>)` after `passed` when the disk suite never started.

**Symptom**
None on the line itself — a latent verification gap. A boot without a disk printed the same `routing_checks: passed` as a full disked run: the three disk checks skip quietly (each with its own line, none consolidated), and nothing in the verdict distinguished the two. The gap ate a regression whole: the block stack had been dead since 40a29b42 (`virtio-blk` could not map its registers), every disked check skipped, and `routing_checks: passed` kept printing — the 261102 TSC record's "Disked: routing_checks: passed" was one of these hollow passes.

**Cause**
The skip is the right answer to "no disk in this image" — `check_block` answers the question "is the route even there", and the images boot_check boots by default carry none. What was wrong was the verdict's shape: `passed` is the only greppable fact, and a required check that passes vacuously is indistinguishable from one that tested what it names. The rule the fix follows: a skip may exist, but it must be observable at the verdict, in one line, with its reason.

**Change**

```cpp
// the verdict, after `routing_checks: passed`
if (!DISK_CHECKS_RAN) {
    auto _ = ort::debug_line({"routing_checks: disk checks not run (",
                              BLOCK_DEVICE_SEEN ? "no mounted volume" : "no block device",
                              ")"});
}
```

`DISK_CHECKS_RAN` is set once `check_disk` has opened `/disk` — the first point past every skip; `BLOCK_DEVICE_SEEN` once `Block.GetInfo` has answered Ok — which splits "no block device" from "device there, no volume served". The per-check skip lines stay as they were; the new line is the one a verifier or a reader greps.

Rejected: making the not-run line a failure (a diskless boot would fail every run, and the skip is legitimate); and putting the reason into the `passed` line itself (the required grep in `boot_check.py` matches `routing_checks: passed` exactly, and a line whose tail changes per boot shape is a worse anchor).

**Reference**
None, this project's own design. The closest practice is a test framework's "skipped, reason" verdict beside its pass count — pytest's `SKIPPED` section, googletest's `--gtest_filter` absence being visible in the counts — applied to a boot log.

**Verification**
Diskless `boot_check.py`: dbg PASS (45), rel PASS (43, 7/7 required), and rel's log reads `routing_checks: passed` with `routing_checks: disk checks not run (no block device)` under it. Disked: rel PASS, `routing_checks: passed`, and no not-run line — the GPT suite ran. The two shapes are the whole of the flag's space: the device and the volume are present or not.

**Deliberately not done**
- Enforcement, not just observation: `boot_check.py` could fail a disked run whose log carries the not-run line. That belongs to the verifier, which knows whether `DISK` was set; it is filed as the outstanding item in `docs/repairs/261102/routing-checks.md`.
- A third state for "device there, GPT there, btreefs exited": `DISK_CHECKS_RAN` covers the mounted filesystem only, which is what the disk suite checks; the block and GPT checks report their own skips.

**How it was found**
The regression itself: bisecting the dead block stack while `routing_checks: passed` kept printing on every boot, disked or not. The line is the answer to "what would have caught this on the day it landed".

**The user's prediction**
None. The user approved the signal and its filing.

**Three questions you should be able to answer**
- Why is `BLOCK_DEVICE_SEEN` set on `GetInfo`'s reply and not on the route's `connect` — what does an epitaph on a connected channel tell you that a closed route does not?
- The diskless pass now prints two facts, `passed` and `not run`. Which one does `boot_check`'s required list consume, and why must the other stay outside it?
- `check_gpt` can skip ("no GPT on the disk") without clearing `DISK_CHECKS_RAN`'s opposite — what state does the verdict line claim, and what does it deliberately not claim?
