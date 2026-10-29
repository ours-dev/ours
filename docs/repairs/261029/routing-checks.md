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
