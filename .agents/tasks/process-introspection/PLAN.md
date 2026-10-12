# process-introspection: the steps from process maps to a system-wide symbolized profile

**Date**: 2026-10-12
**Status**: Implemented

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
Sources listed; each step also changed the headers that declare them.
- Step 1 (2fcdf325): `system/public/ours/object_info.hpp`, `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/object.cpp`, `kernel/object/process_dispatcher.cpp`, `kernel/object/vmo_dispatcher.cpp`, `kernel/mm/vm_aspace.cpp`, `kernel/mm/vm_area.cpp`, `kernel/mm/vm_object_paged.cpp` (with `vm_mapping.hpp`, `vm_cow_pages.hpp`, `vm_page_map.hpp`), `system/app/shell/shell_tests.cpp`.
- Step 2 (007a72c1): `system/public/ours/syscall.hpp`, `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/process.cpp`, `kernel/object/process_dispatcher.cpp`, `kernel/mm/vm_aspace.cpp`, `kernel/mm/vm_mapping.cpp`, `system/app/shell/shell_tests.cpp`; a983516f fixed the shell's vDSO layout reader it exposed.
- Step 3 (376d5cd5): `system/public/ours/object_info.hpp`, `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/object.cpp`, `kernel/object/job_dispatcher.cpp`, `kernel/object/process_dispatcher.cpp`, `system/app/shell/shell_tests.cpp`.
- Step 4 (a319b06b): `system/lib/ours.resource/include/ours_resource/ours_resource.hpp`, `system/service/init/init.cpp`, `system/service/init/realm.cpp`, `system/service/init/pools.hpp`, `kernel/products/image/dev/BUILD.gn`, `system/app/shell/BUILD.gn`, `system/app/shell/shell_tests.cpp`.
- Step 5 (064dfa46): `system/public/ours/exception.hpp`, `kernel/include/ours/task/exception.hpp`, `kernel/lib/syscall/src/exception.cpp`, `kernel/object/job_dispatcher.cpp`, `kernel/object/thread_dispatcher.cpp`, `kernel/object/exceptionate.cpp`, `kernel/object/process_dispatcher.cpp`, `system/app/shell/shell_tests.cpp`.
- Step 6: `examples/profile/describe.cpp`, `examples/profile/describe.hpp`, `examples/profile/profile.cpp`, `examples/profile/BUILD.gn`, `system/app/shell/shell_tests.cpp` (a comment).

## Test plan
The shell checks of DESIGN.md's Testing section, one line each, `shell: <topic> checks passed` on both images where the handles exist; `profile:` reporting the processes it described.
