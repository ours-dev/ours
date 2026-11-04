# pmm-transfer: design the PMM operation boundary

**Date**: 2026-11-04
**Status**: Proposed

## Problem

The allocation path now separates candidate eligibility, zone withdrawal, and delivery to the caller. The memory-only NUMA regression verifies that a request whose remote home is exhausted can obtain a local page and increment the actual local-hit counter.

The zone's buddy and the free-page count still change in separate steps. With an empty zone, CPU A can publish a returned block in the buddy, CPU B can take it and decrement the count, and CPU A can increment the count afterwards. A lock-free reader between the two updates can observe a wrapped unsigned count. A second seam remains at operation completion. Bulk and fragment allocation each own a rollback loop, and the `mm::` facade samples pressure after operations while direct static `PmNode` callers bypass that sample.

## Goals and non-goals

- Keep `PmFrame`, `PmZone`, `PmNode`, `Gaf` and the static node entry points as the public vocabulary.
- Give each zone one inventory mutation boundary and each batch one ownership handoff.
- Run pressure observation at a named completion point, with admission and waiting exposed through the existing `mm::` facade.
- Preserve the existing request errors, placement constraints, batch geometry, bootstrap handoff and reclaim ownership.

## Design

### Commit stock in PmZone

In the buddy path, update the free-page count under the same IRQ-save zone lock as `take_block()` and `return_block()`. The lock-free count is then a snapshot of a completed zone update. A sum across zones is still approximate because the samples are taken separately. Zeroing stays after withdrawal and before delivery, outside the zone lock. A future per-CPU cache needs to state which inventory its count includes and where a block changes owner.

Zircon's `alloc_page()` removes the page and decrements the count with the same lock token, then checks its fill pattern outside the critical section. The local design uses that ownership rule with zone granularity. [Zircon page allocation](</Users/smallhuazi/Workspaces/projects/fuchsia/zircon/kernel/vm/pmm_node.rs:708>)

### Hold batch ownership until delivery

Keep the bulk and fragment order loops visible. Bulk supplies order zero for each page and fragment supplies one order per set bit in ascending order. Both feed the same per-block withdrawal. Use the existing `PmFrame::List` and a private `gktl::defer` rollback that returns each block by its descriptor's zone. Cancel the rollback when ownership of the completed list passes to the caller. The caller sees a complete list or an error after all staged blocks have been restored. Other CPUs can observe intermediate stock because each block transfer takes its zone lock independently.

The resolved `AllocRequest` defines which zones may be attempted. A narrower node mask is an intersection with the resolved mask, so derived requests preserve `Gaf::ThisNode`. A change to the home goes through resolution again. Locality counters count successful block withdrawals from the actual supplier, including blocks later returned by batch rollback.

### Observe an operation once

Define a runtime operation as one public static `PmNode` allocation or return, including list and batch forms. Its internal block primitives handle ownership. After preparing a successful result or restoring staged blocks, call one private pressure check before returning, with no zone lock held. Internal rollback and per-block loops use the owner-directed primitive directly. Keep the pressure state in `FreeMemoryGate` and remove the facade's duplicate sampling when the common completion path takes over. The check can return immediately when no watcher is armed, including early boot.

Keep admission and the wait protocol at `mm::try_alloc_frames()` and `wait_for_frame_allocation()`. A direct static request is immediate. An admission check can return `ShouldWait`, and the caller can then wait with a deadline after releasing its object lock. Reclaim remains a consumer of pressure signals. `VmCowPages` still validates a page-queue candidate under its own lock before returning a page to PMM.

```text
mm::try_alloc_frames → FreeMemoryGate::admit → PmNode::alloc_frames
                                                 ↓
            AllocRequest → ZoneList::candidates → PmZone::take_block
                                                 ↓
                               delivery or rollback → pressure check
```

## Alternatives considered

- Keep separate atomic deltas around the buddy lock. This allows the count to diverge from the reachability of the returned block and can invalidate an admission sample.
- Publish pressure on every block transfer. This produces observations during the rollback of a caller-visible batch and repeats the global sample. Observe at the operation boundary.
- Add a public batch owner type. The existing list and a private scope guard express the current ownership contract with a smaller interface.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
| --- | --- | --- | --- | --- |
| Zone accounting | Separate delta or locked stock commit | Commit the count with the buddy mutation | | |
| Batch rollback | Existing list and guard or public batch object | Reuse the list and guard | | |
| Pressure observation | Facade entry or operation completion | Observe direct and facade operations once at completion | | |

## Testing and verification

1. Exercise a zone starting at zero free frames with concurrent return and withdrawal. Every observed per-zone count stays within its managed size, and the final count agrees with the locked buddy walk.
2. Keep the memory-only NUMA scenario for the local-hit counter, strict exhaustion, and recovery of the same frame after bulk and fragment failure. Include a batch with blocks owned by distinct zones to verify return routing.
3. Arm a pressure watcher and exercise direct and facade operations, including successful batches and rollback. Check the existing timeout, refusal and wakeup protocol, and verify that publication follows completion with zone locks released.
4. Boot the default and NUMA dev images and run the host buddy and request tests, `pmzone.test`, `pmnode.test --exhaust-remote` and `mempressure.test`.

## Open questions

- Which inventory the unfinished per-CPU cache should publish when it becomes active.
