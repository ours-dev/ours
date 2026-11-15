# mm changes

## The frame's packed word is page::StatusWord (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `page::Status` renamed `page::StatusWord`, with a comment saying what the word holds

**Symptom**
Inside `namespace ours::mm::page`, an unqualified `Status` named the frame's packed bit-field word, while every mm operation, `pin_frame()` and `unpin_frame()` among them, returns `ours::Status`. Nothing misused it yet; a function added to `page::` that returned `Status` would have returned a bit-field word.

**Cause**
The alias was named for what it describes (the frame's status) at a time when nothing in `page::` returned a result. The name hides the outer one by C++'s ordinary scope lookup: the innermost declaration wins.

**Change**
`using StatusWord = ustl::BitFields<...>`, and the member `page::StatusWord m_status`. The member keeps its name: it is the frame's status, and only the type collided.

**Reference**
None: a name of this codebase's own.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed.

**Deliberately not done**
None.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor findings, naming.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a type alias in a nested namespace hide one with the same name in an enclosing namespace, and how would `ours::Status` still be reached?
- Why rename the type and not the member?
- What would a compiler have said if a `page::` function had declared `-> Status` and returned `Status::Ok`?

## resolve_request's node set is |nodes|, and three test locals say what they hold (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — `resolve_request()`'s parameter `fallback` is `nodes`, in the friend declaration and the definition
- `kernel/tests/pm_node_test.cpp` — `pinned` is `home_excluded` (a ThisNode request whose node set leaves out its home) and `this_node`, `pinned_failed` is `this_node_failed`, `invalid` is `fragment_refused`

**Symptom**
`resolve_request(..., NodeMask const &fallback, ...)` receives the caller's allowed node set, which every caller names `nodes`, while "fallback" already means the zone order `ZoneList::fallback()`. In the tests, `pinned` named a request confined by `Gaf::ThisNode`, since `pin_frame()` gave "pin" a meaning of its own, and `invalid` held a check for `Status::OutOfMem`, left from when the expected answer was `InvArgs`.

**Cause**
Names kept through changes in what they held: the mask was once the fallback order's input, the test requests were "pinned to a node" before frames could be pinned, and the fragment check changed its expected status without changing its name.

**Change**
The renames above; no behaviour changes.

**Reference**
None.

**Verification**
`verify` on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed (`alloc-request-smoke` among them); `ktest sel=PmNode.*` on four nodes 11 ran, 10 passed, 1 skipped.

**Deliberately not done**
None.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor findings, naming.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What two different things would "fallback" have meant in one call from `alloc_block()` to `resolve_request()`?
- Why is a stale name a defect here rather than a cosmetic issue?
- Which test check named `invalid` would have read as a bug report had it failed?

## The anonymous-page wait carries task::Interruptible, not a bool (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `wait_for_frame_allocation()` and `FreeMemoryGate::wait_for_opening()` take `task::Interruptible`; the gate's bool-to-enum conversion goes
- `kernel/mm/include/ours/mm/page_request.hpp`, `kernel/mm/page_request.cpp` — `AnonymousPageRequest::allocate()` takes `task::Interruptible`; `MultiPageRequest::wait()` converts its `suspendable` bool once, with a comment saying the two words mean the same
- `kernel/tests/memory_pressure_test.cpp` — the three calls pass `task::Interruptible::Yes` or `No` instead of `true` or `false`

**Symptom**
One fact had two names and a type that said neither: `MultiPageRequest::wait(bool suspendable)` passed it to `AnonymousPageRequest::allocate(bool interruptible)`, then to `wait_for_frame_allocation(..., bool interruptible)`, and the gate turned the bool back into the `task::Interruptible` the event takes. A call read `wait_for_frame_allocation(flags, deadline, true)`.

**Cause**
The anonymous-page wait was added beside the page-source wait, whose provider interface (`PageProvider::wait_on_event(event, bool suspendable)`) already carried the bool, and the bool was passed along.

**Change**
The mm side of the anonymous-page wait takes the enum from `MultiPageRequest::wait()` down to the event. The page-source side keeps its `bool suspendable`: it is a provider interface with implementations outside mm, and changing it is a separate step.

**Reference**
Zircon's `PageRequest::Wait(bool suspendable)` uses the same word for the same thing; the enum is this kernel's own (`kernel/include/ours/task/types.hpp`).

**Verification**
`verify` on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed; the failure-injection build builds. `ktest all` on the default machine 90 ran, 83 passed, 7 skipped; `ktest sel=MemoryPressure.*:Pager*:PageSource*:Vmo*` 7 ran, 7 passed.

**Deliberately not done**
- `PageProvider::wait_on_event()` and `MultiPageRequest::wait()` still take `bool suspendable`.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor findings, naming.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does a bare `true` at a call site fail to tell its reader that `task::Interruptible::Yes` does?
- Why convert once at `MultiPageRequest::wait()` rather than change the provider interface too?
- What ends an interruptible wait for a frame, and what does the caller see then?

## ARCH_MAX_PFN_LIMIT is spelled ARCH (2026-11-15)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/mem_cfg.hpp` — `ARHC_MAX_PFN_LIMIT` renamed `ARCH_MAX_PFN_LIMIT`, its definition and its one use

**Symptom**
The Normal zone's bound beside `ARCH_MAX_DMA_PFN` and `ARCH_MAX_DMA32_PFN` was spelled `ARHC_MAX_PFN_LIMIT`, so a search for the arch's zone bounds by their prefix missed it.

**Cause**
A typo from the first commit.

**Change**
The rename; the value (64 GiB) is unchanged.

**Reference**
None.

**Verification**
`verify` on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed.

**Deliberately not done**
None.

**How it was found**
Noticed beside the Dma32 bound on 2026-11-14 and left for its own change.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a misspelled macro still compile and work when its one use is spelled the same way?
- What would a search for `ARCH_MAX_` have told a reader about the zone bounds before this change?
- Why is a bound like this a macro at all here, when the codebase defines constants with `constexpr`?
