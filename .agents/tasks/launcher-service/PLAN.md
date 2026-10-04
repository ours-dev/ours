# launcher-service: three commits from launch-it-yourself to one launcher

**Date**: 2026-10-04
**Status**: Accepted

## Request
The user's decision, 2026-10-03: an independent launcher, handed VMEX by init, through which every program and service is started. On 2026-10-04 the user asked to go straight to the work, so the two open questions were settled as recommended (DESIGN.md). The scope ends when, in the dev image, only the launcher holds VMEX and every start after the launcher's own is a request to it.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md | See DESIGN.md | See DESIGN.md | None given | 2026-10-04 |

## Steps
1. `feat(launch)`: the protocol and the client, in the launch library.
   - `<launch/protocol.hpp>`: the Launch and Connect requests and the reply, with an encoder and a decoder for the Launch request that make no system call, so the host tests can reach them.
   - `<launch/client.hpp>`: `Client::launch()` and `connect()`.
   - Verify: host tests of encode and decode, including every refusal; both images still boot (nothing uses it yet).
2. `feat(launcher)`: the launcher program, and init starting everything through it.
   - `system/service/launcher`: the port loop, Launch and Connect.
   - init starts it with the library, gives it a duplicate of VMEX, and starts the shell and the autorun command with Launch requests. The shell still receives VMEX in this step.
   - Verify: both images boot; the boot log shows the launcher starting the shell; the autorun build starts its program.
3. `feat(init,shell)`: only the launcher holds VMEX.
   - init closes its VMEX and executable BootFS after starting the launcher, and gives the shell a connection (Connect) instead of VMEX.
   - The shell launches its commands over the connection when it has one, and with the library otherwise (dbg_1).
   - Verify: the dev shell says it started without vmex; commands typed over serial run; dbg_1's probes still run; the shell checks a NotFound and a malformed request round trip.

## Files
- `system/lib/launch/include/launch/protocol.hpp`, `client.hpp`, `src/protocol.cpp`, `src/client.cpp`, `BUILD.gn`, `test/` — step 1.
- `system/service/launcher/` (new), `system/service/init/init.cpp`, `BUILD.gn`, both images' `BUILD.gn` — step 2.
- `system/service/init/init.cpp`, `examples/shell/shell.cpp`, `shell_tests.cpp` — step 3.
- `docs/changes/261004/launch.md` — a record per step.

## Test plan
As DESIGN.md's testing section, after each step.
