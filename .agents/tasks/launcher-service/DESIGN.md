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
To be designed. The decided shape:
1. userboot starts init with the library, as today.
2. init starts `launcher` with the library and gives it VMEX, the BootFS and the vDSO's VMO. Then init closes its own VMEX and executable BootFS.
3. Every later start (the shell, init's startup list, autorun, the shell's commands and fault probes) is a request to the launcher over a channel. The launcher creates the process, writes its processargs, and replies with the process handle.

Zircon/Fuchsia counterpart: component_manager serves `fuchsia.process.Launcher` (`src/sys/lib/elf_runner/src/process_launcher.rs`), built on the in-process `ProcessBuilder`; clients reach it through `fdio_spawn`. The difference is that in Fuchsia the launcher does not hold VMEX: clients hand it VMOs that are already executable.

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

## Testing and verification
To be designed. At least:
- after boot, only the launcher holds VMEX; init and the shell get `NotFound` from `take_startup_handle(SystemResource, VMEX)`;
- every program start in the boot log goes through the launcher;
- the shell's fault probes and commands still run.

## Open questions
- The request format: a fixed struct per message, or Fuchsia's sequence of Add* messages before Launch.
- Whether the launcher also returns the new process's root VMA, which nothing outside the launcher needs today.
