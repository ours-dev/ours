# launcher-service: one launcher process starts every program, and only it holds VMEX

**Date**: 2026-10-04

## Delivered
- `3b57470b` feat(launch): the protocol (`<launch/protocol.hpp>`) and the client (`<launch/client.hpp>`), with host tests of every refusal (step 1).
- `7e680d98` feat(launcher): `bin/launcher`, serving Launch and Connect on a port; init starts it with the library and then everything else through it (step 2).
- `b6d44cb3` feat(init,shell): init closes its VMEX after starting the launcher; the dev shell starts its commands over a connection and holds no VMEX (step 3).

## Verification
- `boot_check.py` after every step, and on HEAD plus only that step's files in a scratch copy, since the shared tree held other sessions' work: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped) at the end.
- The rel log: "init: vmex is the launcher's alone", "launcher: started bin/shell", "shell: started without vmex", "shell: launcher service checks passed".
- Over serial on the dev shell: `ls`, `fault_probe none` (started by the launcher, on the autorun build), an unknown command (NotFound), and a stray `launcher` (exits; the prompt comes back).
- The autorun build starts its program through the launcher.
- A review workflow after each of steps 2 and 3; every standing finding fixed, each step's change record says which.

## Deviations from the design
- The design's testing section asked that init and the shell get NotFound from `take_startup_handle(SystemResource, VMEX)`. The shell is never given one, and says so ("started without vmex"); init closes its own after starting the launcher and says so. Neither asks the runtime a second time, since a startup handle is taken once.
- The dbg_1 image is unchanged: its shell, started by userboot with no init and so no launcher, still holds VMEX and uses the library.
- `MAX_STRINGS` (64 of each kind) became part of the protocol in step 2, after the review found it hidden in the launcher.

## Found along the way
- `tools/set.py`'s autorun check had been broken since `e8eaf1be` (fixed in `8bfa2a79`, before this task).
- The rel image has had no kernel shell since the interactive user shell (`ead5d12f`); kernel tests run on dbg only (`20fadafc`).

## Not done
- An event-loop library (U10): the launcher's loop is its own.
- init's startup list is still only packaged; init starts the shell by name.
- A typed `init` still halts and holds the shell (init halts without the root job), as before this task.
