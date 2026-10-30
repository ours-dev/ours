# routing-checks outstanding repairs

## 1. The comments of routing_checks count its protocol uses wrongly and leave out ours.resource.Sampling

**Severity**
Low: a stale comment. Nothing runs differently, but a reader who counts the uses from the comments finds fewer than the manifest has, and the file is the model that the developer's guide sends readers to.

**Symptom**
`system/service/init/test/routing_checks/routing_checks.cpp:24-27` says the manifest "also uses five protocols" and names `ours.process.Launcher`, `ours.resource.Debug`, `ours.resource.Irq`, `ours.resource.Ioport` and `ours.test.Unoffered`. Line 38 calls `ours.test.Echo` its "sixth protocol use". `system/service/init/test/routing_checks/BUILD.gn:38-48` says "And six protocol uses" and lists the same five and `ours.test.Echo`. The manifest in the same file (`BUILD.gn:75-89`) names six protocols in its first clause, `ours.resource.Sampling` among them, and `ours.test.Echo` in a second, so it has seven uses. `check_sampling_resource()` (`routing_checks.cpp:472`) is called from `main()`, so the program checks a protocol that neither comment mentions.

**Cause**
`ours.resource.Sampling` was added to the manifest and to the program, and the two comments that count and list the uses were not updated with it.

**Change**
Say seven protocol uses in both places, add `ours.resource.Sampling` to both lists with the other resource protocols the root offers, and call `ours.test.Echo` the seventh. Do not renumber by hand in a third place: `main()` and the manifest are the lists, and the comments should name them rather than repeat the count where that reads as well.

**Reference**
None, this project's own tests.

**Acceptance**
`grep -n "five protocols\|sixth protocol\|six protocol" system/service/init/test/routing_checks/routing_checks.cpp system/service/init/test/routing_checks/BUILD.gn` finds nothing, and the protocol names in the comments are the ones in the manifest. `python3 .agents/skills/ours-verify/scripts/boot_check.py` still passes both images with `routing_checks: passed`.

## 2. A check that expects a refusal waits for ever when the server accepts instead

**Severity**
Low: a regression is still caught, but as a timeout. Nothing is wrong while every server refuses as it should.

**Symptom**
`connect_status()` (`system/service/init/test/routing_checks/routing_checks.cpp:770-788`) and `Incoming::receive()` (`:343-357`) wait for the channel to be readable or closed. A server that should refuse a client but takes it writes nothing, so the wait never ends: the check does not print a `FAILED` line, `routing_checks: passed` never comes, and `boot_check.py` fails the image after 60 s with `no "routing_checks: passed"` among the lines it names. Seen on 2026-10-29 in a copy of echo whose client table was larger than the check's second client assumed.

**Cause**
`object_wait_one` takes a deadline of 0 or `INSTANT_INFINITE` and answers `NotSupported` to any other, "until a program can read a clock" (`system/public/ours/syscalls.gni:301-303`), so a check has no way to give up on a wait. The same holds for every negative check in the file that waits for an epitaph.

**Change**
Not decided. The options: (a) leave it: the missing required line names what did not run, and the guide's first-server chapter says what the hang looks like; (b) when a program can read a clock, give `receive()` a deadline and make a timeout a `FAILED` line. Ruled out: polling with deadline 0 in a loop, which spends the one thread's CPU and still needs a limit on the loop.

**Reference**
None, this project's own tests.

**Acceptance**
A check that expects an epitaph from a server that accepts the client prints a line with `FAILED` in it within a few seconds, instead of leaving `boot_check.py` to time out.
