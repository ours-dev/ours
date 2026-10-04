# launcher-service: one launcher process starts every program, and only it holds VMEX

**Date**: 2026-10-03
**Status**: Accepted

## Problem
Today every process that starts programs links `//system/lib/launch` and holds what launching takes: VMEX, the BootFS and the vDSO's VMO. That is userboot, init and the shell. Each maps the BootFS and makes an executable copy of its own.

VMEX lets its holder make any VMO executable. Design decision D3 (`docs/design/260923/userspace-interface.md`) meant init alone to hold it. The interactive-shell work (`ead5d12f`) passes it on to the shell as well, so three processes hold it today.

## Goals and non-goals
Ordered after the boot-image work (route B), LZ4, U9 and the ports (K26 to K28), decided 2026-10-03.

The user's decision, 2026-10-03:
- The launcher is an independent process.
- init creates it, then hands it VMEX. From then on the launcher is the only holder of VMEX in the system.
- Every program and service is started through the launcher.

Non-goals:
- userboot keeps its own loader. It is the first program, before any service exists; Zircon's userboot has its own `launch_program()` too.
- init starts the launcher itself, with the library, as its one use of VMEX.

## Design
The decided shape:
1. userboot starts init with the library, as today.
2. init starts `launcher` with the library and gives it VMEX, the BootFS and the vDSO's VMO. Then init closes its own VMEX and executable BootFS.
3. Every later start (the shell, init's startup list, autorun, the shell's commands and fault probes) is a request to the launcher over a channel. The launcher creates the process, writes its processargs, and replies with the process handle.

Zircon/Fuchsia counterpart: component_manager serves `fuchsia.process.Launcher` (`src/sys/lib/elf_runner/src/process_launcher.rs`), built on the in-process `ProcessBuilder`; clients reach it through `fdio_spawn`. The difference is that in Fuchsia the launcher does not hold VMEX: clients hand it VMOs that are already executable.

The objects (worked out 2026-10-04):
- **The launcher**, `bin/launcher` (`system/service/launcher`). Startup handles: User0, its first client channel (init's); VmoBootfs; SystemResource VMEX; VmoVdso; VmaRoot. It makes one `launch::BootfsLauncher` and keeps it for every launch.
- **A connection**: one channel. The launcher serves every connection on one port: each client channel is watched with `object_wait_async` for Readable and PeerClosed, re-armed after each message, and closed on PeerClosed. One thread, one request at a time; a request is answered before the next is read.
- **The protocol** (`<launch/protocol.hpp>`), two requests:
  - **Launch.** Bytes: a header (ordinal, the BootFS name's length), the name, padding to 4 bytes, then a processargs message exactly as the child should receive it, args, environ and names, but without VmaRoot. Handles: the job first, then the child's startup handles in the order of that processargs' handle_info. The launcher checks the processargs with `ort::ProcessArgs::parse()`, starts the program under the job, appends VmaRoot and writes the processargs on the child's bootstrap channel. Reply: the ordinal and a Status; on Ok, the process handle.
  - **Connect.** Bytes: the header alone. Handles: one channel end, which becomes a new connection. No reply. This is how init gives the shell a connection of its own without the shell ever holding anything of the launcher's but that.
- **The client** (`<launch/client.hpp>`): `launch::Client::launch()` encodes a Launch request from a name, argv, environ and handles, writes it, waits for the reply and returns the process handle; `connect()` makes a channel pair, sends one end in a Connect request and returns the other. The job is duplicated for the request, so the caller keeps its own.

The events, at boot of the dev image:
1. userboot starts init (library, VMEX handed on).
2. init starts the launcher with the library, giving it VMEX, the BootFS and the vDSO; init closes its own VMEX and its executable BootFS copy.
3. init sends Launch for `bin/shell`, with a liveness channel (User0), a duplicate of the job, the BootFS (for `ls`), the vDSO VMO (for the shell's vDSO-mapping check), the Debug resource, and a connection of the shell's own (User1, from Connect).
4. The shell's commands go to the launcher over that connection. The shell holds no VMEX.

The dbg_1 image is unchanged: its shell is started by userboot directly, holds VMEX and launches with the library, because that image has no init and so no launcher.

## Alternatives considered
- Init as the launcher service (rejected by the user, 2026-10-03): the launcher must be a process of its own.
- The library in every starter (today): VMEX spreads to every process that starts programs.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Where launching lives | (a) an independent launcher process holding VMEX alone; (b) init serves launches | (b) was proposed | None | (a), 2026-10-03, the user's |
| What a request names | (a) a BootFS file name, resolved in the launcher's own BootFS; (b) a VMO the client supplies, made executable by the launcher | (a): only boot-image code can ever run, and VMEX is never used on a client's VMO | None given | 2026-10-03, as recommended |
| Who writes the processargs | (a) the launcher, from the argv, environment and handles in the request, adding VmaRoot itself; (b) the client, on a bootstrap end it gets back | (a), as `fuchsia.process.Launcher` does | None given | 2026-10-03, as recommended |
| Which job | (a) the client passes the job the process goes under; (b) the launcher's own | (a), as `LaunchInfo.job` in Fuchsia | None given | 2026-10-03, as recommended |
| Serving several clients | (a) on ports and the Loop (K26 to K28, already decided as D4 of `docs/design/260924/userspace-filesystem.md`); (b) `object_wait_many` first | (a): D4 deferred `object_wait_many` to P2 | None given | 2026-10-03, as recommended |
| The request format | (a) one message per launch: a header, the name, and the child's processargs minus VmaRoot; (b) Fuchsia's sequence of Add* messages before Launch | (a): the child's processargs must fit one message anyway, so a request that adds a name fits too; no per-connection state; the existing encoder and decoder do the strings and handle infos | None given | 2026-10-04, as recommended: the user asked to go straight to the work |
| Whether the reply carries the root VMA | (a) the process handle only; (b) the process and its root VMA | (a): the launcher writes VmaRoot into the child's processargs itself, and nothing outside it needs the region; `fuchsia.process.Launcher` returns the process only | None given | 2026-10-04, as recommended: the user asked to go straight to the work |
| How a client gets a connection | (a) a Connect request carrying a channel end, sent by whoever already has a connection (init); (b) a startup handle per client from init at the launcher's start | (a): clients are started after the launcher | None given | 2026-10-04, as recommended |

## Testing and verification
- After boot of the dev image, only the launcher holds VMEX: init closes its own after starting the launcher, and the shell is started without one (its "started without vmex" line).
- Every program start in the dev image's boot log after the launcher's own goes through the launcher, which says so for each.
- The shell's commands still run (typed over serial), and its fault probes still run in dbg_1.
- The protocol's refusals: a request for a name the BootFS lacks is NotFound; a malformed request is InvArgs and leaves the connection usable; a closed client is dropped.
- Host tests for the request encoding (`system/lib/launch/test`).

## Open questions
- An event-loop library (U10's Loop) is not written yet; the launcher's loop is its own, a few dozen lines on a port, until U10 extracts one.
