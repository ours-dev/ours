# process-introspection: the steps from process maps to a system-wide symbolized profile

**Date**: 2026-10-12
**Status**: Accepted

## Request
"你看着来，尽可能完善的做" (2026-10-12), after choosing option A of `docs/repairs/261007/sampler.md` item 4 and asking for the process-starting exception channel. The scope ends when `profile` prints markup for every process its records name and prof-fold names their user frames.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Order | Maps and read first; the exception first | Maps and read first: simplest, testable at once, needed by every later step | Not given | Maps and read first, 2026-10-12 (delegated) |
| Where the work happens | This session; a session of its own for exception channels | This session, as delegated | Not given | This session, 2026-10-12 |

## Steps
1. ProcessMaps: `mm::VmAspace` enumeration of areas and mappings, `VmObject` koid, `ProcessDispatcher::get_maps()`, the multi-record helper in `object_get_info`, the topic and record in `object_info.hpp`, shell checks. One commit.
2. `process_read_memory`: the syscall entry, `ProcessDispatcher::read_memory()`, `MAX_PROCESS_READ_MEMORY`, shell checks reading the shell's own ELF header. One commit.
3. Job process list, JobProcesses, `object_get_child` for processes, shell checks. One commit.
4. `ours.kernel.RootJob` served by init and routed to the shell; a dev shell check that gets it and compares its koid with the job it holds. One commit.
5. Job debugger channel and ProcessStarting: ABI, the job's debugger exceptionates, the raise in the starting thread's trampoline, shell checks. One commit.
6. `profile` describes every process through steps 1-5, and prof-fold names other processes' frames; `docs/repairs/261007/sampler.md` item 4 closes. One commit.

Each step: clang-format and clang-tidy on its lines, `ours-verify` in `.worktrees/wip-proc-self` at baseline plus only the step, a mutation where a test guards a line, a change record, and a commit of only its paths.

## Files
To be filled as each step lands.

## Test plan
The shell checks of DESIGN.md's Testing section, one line each, `shell: <topic> checks passed` on both images where the handles exist; `profile:` reporting the processes it described.
