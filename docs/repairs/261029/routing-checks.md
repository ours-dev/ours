# routing-checks outstanding repairs

## 2. A check that expects a refusal waits for ever when the server accepts instead

**Severity**
Low: a regression is still caught, but as a timeout. Nothing is wrong while every server refuses as it should.

**Symptom**
`connect_status()` (`system/service/init/test/routing_checks/routing_checks.cpp:1005-1026`) and `Incoming::receive()` (`:577-595`) wait for the channel to be readable or closed. A server that should refuse a client but takes it writes nothing, so the wait never ends: the check does not print a `FAILED` line, `routing_checks: passed` never comes, and `boot_check.py` fails the image after 60 s with `no "routing_checks: passed"` among the lines it names. Seen on 2026-10-29 in a copy of echo whose client table was larger than the check's second client assumed.

**Cause**
`object_wait_one` takes a deadline of 0 or `INSTANT_INFINITE` and answers `NotSupported` to any other, "until a program can read a clock" (`system/public/ours/syscalls.gni:301-303`), so a check has no way to give up on a wait. The same holds for every negative check in the file that waits for an epitaph, and for the checks that read the directories the sibling `files` provides (`check_files()`): a Read of `/files/hello.txt` waits for `files`' server, so a `files` that runs and never reads its outgoing directory leaves `routing_checks` in `fsio::read()` for good, and so does one whose Subtree handler takes the Open for `docs` and never forwards it to the server of its tree. A `files` that fails to start does not: its server end closes, the root's Open and the entry are destroyed with it, and `open()` answers at once. `refused_by_provider()` allows one `PeerClosed` and asks again, which is two waits on `files`' servers and no more (for a path below `docs` the answer comes from the server of the tree behind the Subtree node, a Loop turn after `files` took the root's Open). The provider-exit case, `check_files_stop()` (`:1121`), has the same two waits: `connect_status()` on the channel of `/files/stop` ends when `files` exits and the kernel closes the channel it was handed, and `Incoming::receive()` on a connection held before ends when the same exit closes that one. A wrong path does not hang, because the server's epitaph is the first wait's answer and the case reports it and stops; a stop node whose handler does not exit leaves the first wait for good, and `boot_check.py` reports `no "routing_checks: passed"` and `no "init: files exited with 0"` after its timeout.

**Change**
Not decided. The options: (a) leave it: the missing required line names what did not run, and the guide's first-server chapter says what the hang looks like; (b) when a program can read a clock, give `receive()` a deadline and make a timeout a `FAILED` line. Ruled out: polling with deadline 0 in a loop, which spends the one thread's CPU and still needs a limit on the loop.

**Reference**
None, this project's own tests.

**Acceptance**
A check that expects an epitaph from a server that accepts the client prints a line with `FAILED` in it within a few seconds, instead of leaving `boot_check.py` to time out.
