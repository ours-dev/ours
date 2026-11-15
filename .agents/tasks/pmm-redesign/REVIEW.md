# pmm-redesign: review of the PMM pipeline, its objects and the pressure subsystem

**Date**: 2026-11-15

## Scope and method
The code of the PMM redesign as it stands on `feat/pmm-redesign` (identical to `baseline/20260824` at `a004d5c5`): `kernel/mm/pmm.cpp`, `pm_node.cpp`, `pm_node_init.cpp`, `pm_zone.cpp`, `node_table.cpp`, `cpu_node_map.cpp`, `memory_reclaimer.cpp`, `memory_watchdog.cpp`, `page_request.cpp` and their headers, `priv/pmm.hpp`, `priv/admission.hpp`, `priv/alloc_request.hpp`, `priv/zone_candidates.hpp`, `alloc_flags.hpp`, and the tests `kernel/tests/pm_node_test.cpp`, `pm_zone_test.cpp`, `memory_pressure_test.cpp`, `kernel/mm/tests/admission_smoke.cpp`, `alloc_request_smoke.cpp`. Some of it predates the redesign (`FreeMemoryGate`, 2026-10-29; the frame and buddy layer); it is in scope because the redesign builds on it, and each finding says where its code came from when that matters.

The user's question was to prefer object-oriented design over procedural. Five reviewers read one slice each (the pipeline, the node/zone/frame objects, the pressure subsystem, correctness, the tests and conventions) against `modern-cpp-development`, `kernel-design` (`references/mm.md`) and `.agents/tasks/pmm-redesign/DESIGN.md`; a second reviewer then checked every finding against the source and confirmed, narrowed or refuted it. 69 findings were reported, one was refuted (it presented a recorded decision as a defect) and 21 were narrowed. Nothing here was reproduced at run time: the findings are read from the code and confirmed by that second reading, not by a failing test. clang-format (`prebuilt/clang/llvm-23`) reports one line, `pmm.hpp:94`, written on 2026-10-28 before the redesign; clang-tidy reports two old warnings in the headers (`pm_node.hpp:119` `fallback` without `[[nodiscard]]`, `pm_zone.hpp:29` `lists_`).

Several OOP changes below would revisit a recorded decision. Each says which: D1(b) (the `mm::` free functions are the pipeline), the flat `node_*` functions over file-scope data (pmm-layering D2(a)), and the zone table inside `PmZone` (`docs/design/261029/zone-table.md`). Those are the user's to reopen, not defects.

## What is done well
- `AllocRequest` is an immutable value with one builder: a private constructor, `resolve_request()` as its only friend, `with_nodes()` and `with_zone_range()` deriving new values, and `is_confined()` answered by the request itself (`kernel/mm/include/ours/mm/priv/alloc_request.hpp:38`).
- `PmZone::free_frames()` tells instead of being asked: it reports the Low crossing from its own `fetch_add`, so of racing releases exactly one sees it (`kernel/mm/pm_zone.cpp:248`).
- The short-zone record has one owner and a clean dependency direction: one atomic word, `mark_short()` and `take_reclaim_target()`, and the node never calls up into the reclaimer (`kernel/mm/pm_node.cpp:141`).
- `ZoneCandidates` is a real range that never rests on a rejected zone, and keeps eligibility apart from admission (`kernel/mm/include/ours/mm/priv/zone_candidates.hpp:32`).
- `MemoryWatchdog` takes its reclaimer through its constructor and guards its levels and declaration with `TA_GUARDED(m_lock)`; `Pause` and `Hold` are RAII scopes, and `Hold` is a `TA_SCOPED_CAPABILITY` that lockdep sees (`kernel/mm/include/ours/mm/memory_watchdog.hpp:185`).
- The admission arithmetic is pure, `constexpr` and host-tested with real machine numbers, and matches Linux `zone_watermark_ok()`, `__setup_per_zone_wmarks()` and `setup_per_zone_lowmem_reserve()` (`kernel/mm/include/ours/mm/priv/admission.hpp:38`).
- The counters stay at file scope for a reason: `GKTL_COUNTER` is a link-section descriptor with per-CPU storage and cannot be a member (`kernel/mm/pmm.cpp:213`).

## High severity
### H1. The gate can be left shut with nobody told
`kernel/mm/pmm.cpp:132`. `FreeMemoryGate::admit()` reads the state, the delay level and the free count, and only then compare-and-swaps the state from `Open` to `Shut`. The watcher's `set()` (`pmm.cpp:164`) stores the same value `Open` when it re-arms. So a CAS based on old reads still succeeds after a re-arm: the gate ends `Shut` while free memory sits inside the new bounds and above the new delay level, where no `check_signal()` fires, and the watchdog's deadline is infinite at Normal. Every waiting allocation then answers `ShouldWait`, every woken waiter is put off again, and `AnonymousPageRequest::allocate()` (`page_request.cpp:84`) loops until free memory happens to leave the bounds. The window is a re-arm between one allocation's reads and its CAS, so it is rare, but nothing ends it. The code predates the redesign (2026-10-29); N1 leans on the gate more, since every ordinary wait now ends there. Zircon trips and signals in one step under the node lock (`pmm_node.rs`, `decrement_free_count_locked()`). Fix: serialize only the rare `Open` to `Shut` transition: on the slow path take `m_watcher_lock`, re-read state, level and free, shut only if the condition still holds, and signal the armed event in that step; or keep a generation beside the state in one atomic word, bumped by `set()`, so a stale CAS fails. Add a test that interleaves `admit()` with `set()`.

## Medium severity
### M1. A Pause does not stop a pass already under way from taking node records
`kernel/mm/memory_reclaimer.cpp:121`. `run()` asks `paused()` once and then `reclaim_node_requests()` takes every node's record; a Pause that begins in between loses those bits, because `reclaim()` returns 0 and nothing marks the node again. That breaks the contract written in `memory_reclaimer.hpp:74` and `mm.md` (N2, this redesign), and can fail `PmNode.AShortZoneIsMarkedOnceAndTakenOnce` on SMP: the previous test's Pause signals the thread as this one starts. Fix: make "may the thread take records now" one decision shared by the take and the Pause: collect the records under a short lock that the Pause constructor also takes (never held across a walk), and put a record back if its walk is refused (`take_reclaim_target()` would return the zone bits beside the count).

### M2. The buddy reads a neighbour's status word while its owner writes it
`kernel/mm/include/ours/mm/priv/frame_buddy.hpp:88`. `is_mergeable()` reads the partner's role, order, zone and node from `PmFrame::m_status`, a plain bit-field word, under the zone lock; the partner's owner writes the same word without that lock (`become()`, `split_block()` at `pmm.cpp:824`, `unwire_frame()` at `pmm.cpp:841`). Writes to one frame are serialized by ownership, so nothing is lost, but the concurrent read is a data race and so undefined behaviour; it is benign today only because no owner ever writes `Role::Pmm`. Linux keeps the buddy marker in `page_type`, written under `zone->lock` and read with `data_race()` (`page_is_buddy()`). Fix: let `PmFrame` own the one fact the buddy reads across owners in a separate atomic byte (relaxed, written by `insert_frame()`/`remove_frame()`, read through `is_free_head(order)`), or make the status word atomic.

### M3. One run of the pipeline is threaded through parameters instead of being an object
`kernel/mm/pmm.cpp:353`. `alloc_block()` takes six parameters, the last an in-out `depleted` accumulator every caller declares; `alloc_frames_bulk` and `alloc_frames_fragment` (`pmm.cpp:622`, `651`) repeat one skeleton; the rule "observe pressure once after completion" is written out at seven sites. The OOP shape is a per-call object on the entry's stack, the role Linux's `struct alloc_context` plays: `begin()` admits and resolves the home once, `take(order)` is today's `alloc_block`, `give(frame)` is `free_block`, and the destructor observes pressure exactly once. The public entries keep their primitive parameters. This revisits the wording of D1(b) (the stages become members of a stack object instead of free functions); its intent, one entry that cannot be bypassed, stays. The second reviewer noted the home would still be held twice (in the object and in the request), so the gain is the skeleton and the observation rule, not the duplication.

### M4. The request does not own what the caller's flags mean
`kernel/mm/pmm.cpp:607`. `priv::alloc_frames()` strips `CanWait` before the request is built and carries it beside it as a `bool`, so `AllocRequest::flags()`'s comment, "What the caller asked for, unchanged" (`alloc_request.hpp:68`), is false on that path. `Gaf` is decoded in six places (`pmm.cpp:236`, `282`, `600`, `pm_node.cpp:94`, `alloc_request.hpp:151`, `231`) where `DESIGN.md:131` allows two. Fix: `resolve_request()` decodes once into answers the request gives: `waits_at_low()`, `floor(AdmissionPass)` handed by the node to the zone, `zeroes()`. Keep `flags()` whole. This builds the design's boundary table as written.

### M5. Adoption keeps buddy logic in the pipeline, and the release path still takes an order
`kernel/mm/pmm.cpp:777`. `adopt_frames()` carves a range by the buddy's alignment rule and hands `free_block(frame, order)` an order the head frame does not record; that one caller is why `PmNode::free_frames()` and `PmZone::free_frames()` still take an order (F2 half built), and why code below the seam can free at an order the head disagrees with. Only a `DEBUG_ASSERT` checks that a carved block lies in one zone (`frame_buddy.cpp:50`). Fix: `PmZone::adopt(start, end)`, the slot `DESIGN.md:143` lists, clips to the zone's span and releases under its lock; then the order parameter goes from both layers. `DESIGN.md:141` built adoption in the seam on purpose for this task, so moving it is a step the design planned, not a correction of it.

### M6. A block's layout has no owner
`kernel/mm/pmm.cpp:808`. `split_block()` rewrites head orders from outside the frame, tail offsets are stamped only by the object cache, and the buddy leaves stale orders in interior frames (`frame_buddy.hpp:105`, `frame_buddy.cpp:18`). So `free_frames(&block[4])` on an order-3 block releases `[4, 8)` at frame 4's stale order while the head still owns all eight, and nothing asserts. No path in the tree does that today; it is a robustness gap. Fix: give `PmFrame` the block operations `DESIGN.md:124` already assigns it (`make_block(order)`, `split(order)`, `clear_block()`, `is_head()`), call them from `prepare_alloc`/`prepare_free`, and have `prepare_free` assert `is_head()`.

### M7. The placement policy is a switch outside the policy, and its output cannot carry a placement
`kernel/mm/pmm.cpp:280`. `apply_thread_policy()` switches over `AllocPolicy::kind()` with every case empty, and outputs only `Gaf &`, which cannot carry a home node or a node set, so Bind and Interleave have no path into the request. Meanwhile every node-less allocation takes the thread's `IrqSave` spin lock and copies an `Arc` that is always null. Fix: `AllocPolicy::place(local, allowed)` returning a home and a narrowed mask, used by the resolver; record the per-allocation lock and copy in `docs/repairs` as a performance item to measure first.

### M8. The node hands out its fallback order mutably
`kernel/mm/include/ours/mm/pm_node.hpp:285`. `zone_list()` returns `ZoneList *`, whose `connect()` and `take_local_zones()` are public, while every allocation walks the list without a lock on the premise that it is written only at boot. `for_each_zone` and `for_each_zone_if` are declared in the header and defined only in `pm_node.cpp`, so a call from any other file fails to link (no caller; the second uses a ternary); `m_connected` has no reader and `friend class ZoneList` grants nothing used. Fix: `PmNode::connect()` private for bring-up, `PmNode::zone(type)` and `for_each_zone()` for readers, `zone_list()` returning `ZoneList const &`, and the dead members removed.

### M9. The frame's role setter is public, so the protection of `Role::Pmm` exists only in a comment
`kernel/mm/include/ours/mm/pm_frame.hpp:330`. `Role::Pmm`'s comment says only the buddy writes it, after a live frame marked `Pmm` was once handed out twice; `set_role(Role)` is public and takes any role. Fix: named transitions (`enter_free_list(order)`, `leave_free_list()`) reachable by the buddy through a passkey, and a boot stamp that refuses `Pmm`.

### M10. "The levels nest" is not an invariant of the levels type
`kernel/mm/include/ours/mm/memory_watchdog.hpp:122`. A private default constructor builds all-zero levels for the watchdog's member, so validity is re-checked in `start()`, `set_levels()` and `covering()` and asserted in `bounds_of()` and `delay_level()`; the init hook repeats `from_boot_options()`'s conversion and floor sum just to log. Fix: `make()` as the only constructor, `ustl::Option<MemoryPressureLevels>` in the watchdog, `from_boot_options()` moved into the init hook. It changes the documented contract that a bad command line reaches `start()` and is refused there (it would be refused at `make()` instead).

### M11. Each draining test repeats the same protocol by hand
`kernel/tests/pm_node_test.cpp:999`. `AdmissionHoldsAtLowThenAtMin` has its own copy of `remote_single_zone_node()`, and four tests repeat the pick, the Pause, the held list, about ten `if (r) { held.push_back(**r); }` and the final free; an early return after a drain would leave a remote zone drained. Fix: a test-local RAII `ZoneTrial` (zone, Pause, held list, in that order) with `drain_to()`, `keep()` and a destructor that gives everything back. Not a `KTEST_F` fixture: those are one static instance (`fixture.hpp:331`), which would hold the Pause for good.

### M12. The design's own frame-layout test plan is not covered
`kernel/tests/pm_node_test.cpp:860`. `DESIGN.md:271` asks that a block split to order 0 and freed frame by frame through the seam coalesce back, and that freeing restore the zone's free-block profile; `SplitBlockMakesBlocksThatAreFreedAlone` says itself that its frees would not catch a mistake, and the contiguous test does not check its surplus frame came back. Fix: a `FreeBlockProfile` value type (from `pm_zone_test.cpp`'s `count_free_blocks`) compared around a split and a contiguous allocation on a quiet remote zone.

## Minor and style
Naming first.

- page::Status, the frame's packed word, shadows ours::Status inside namespace page: `kernel/mm/include/ours/mm/pm_frame.hpp:112`.
- A 'suspendable' wait is passed on as 'interruptible', as a bool where task::Interruptible exists: `kernel/mm/page_request.cpp:111`.
- Misleading names: resolve_request's 'fallback' mask, 'pinned' and 'invalid' locals: `kernel/mm/include/ours/mm/priv/alloc_request.hpp:228`.
- Two by-name counter lookups and two interrupts-off read-arounds are written by hand; one runs a CanBlock allocation with interrupts off: `kernel/tests/pm_node_test.cpp:598`.
- alloc_frames_contiguous observes pressure once per surplus frame, not once per call: `kernel/mm/pmm.cpp:743` (the second reviewer confirmed only part of it).
- The machine-wide zone sums are free functions over a static table inside PmZone: `kernel/mm/pmm.cpp:40`.
- The pin rules are split between the seam and the frame: `kernel/mm/pmm.cpp:788`.
- unwire_frame does both halves of the reserved books from outside both owners: `kernel/mm/pmm.cpp:841`.
- Nothing owns the machine's free count; the gate recomputes it on every allocation and free: `kernel/mm/pmm.cpp:157`.
- The per-CPU frame cache stub would leak frames and dereference an empty PerCpu if its constant were raised: `kernel/mm/pm_zone.cpp:257` (the second reviewer confirmed only part of it).
- The node and zone free_frames() still take an order the head frame already records (F2 half done): `kernel/mm/include/ours/mm/pm_node.hpp:226` (the second reviewer confirmed only part of it).
- A block's shape (head order plus tail offsets) is two public setters with no owner, so split_block will break once the allocator stamps tails: `kernel/mm/include/ours/mm/pm_frame.hpp:298` (the second reviewer confirmed only part of it).
- The reserved count is one fact written in two halves by callers: the frame flag and the zone counter: `kernel/mm/include/ours/mm/pm_zone.hpp:211` (the second reviewer confirmed only part of it).
- PmNode decides whether a zone is short of High from two zone getters, in two places: `kernel/mm/pm_node.cpp:110` (the second reviewer confirmed only part of it).
- PmZone::Flags::BelowHighWmark has no writer but gates every allocation, at a line its own comment calls wrong: `kernel/mm/pm_zone.cpp:197` (the second reviewer confirmed only part of it).
- CpuNodeMap's private base hides nothing, and the CPU-to-node invariant has public bypasses on both sides: `kernel/mm/include/ours/mm/cpu_node_map.hpp:54` (the second reviewer confirmed only part of it).
- The node table's invariants are spread over public free functions, PmNode::init() and bring-up: `kernel/mm/node_table.cpp:16` (the second reviewer confirmed only part of it).
- PmNode's static bootstrap, setup_watermarks and create_nodes touch no node, and one is there only to be PmZone's friend: `kernel/mm/include/ours/mm/pm_node.hpp:189`.
- Two-phase construction: PmNode::init() finishes the node after registration and reaches into PmZone to set m_node, which nothing reads: `kernel/mm/pm_node_init.cpp:307`.
- PmNode reads Gaf::CanBlock, a third Gaf reader the design rules out: `kernel/mm/pm_node.cpp:94`.
- The pin rules are split between mm::pin_frame/unpin_frame and PageFrame, with the zero check made twice: `kernel/mm/include/ours/mm/pm_frame.hpp:411`.
- BlockFailure carries a status that is always OutOfMem: `kernel/mm/include/ours/mm/pm_node.hpp:199`.
- Duplicated logic: init() repeats join_hard_affinity, and claim_unplaced_cpus is a pure forward: `kernel/mm/pm_node_init.cpp:297`.
- Dead or duplicated state on the node and zone objects: `kernel/mm/include/ours/mm/pm_zone.hpp:153`.
- Teaching: seal the layers with a passkey instead of a blanket friend: `kernel/mm/include/ours/mm/pm_zone.hpp:328`.
- take() is public only so the watchdog can enforce the reclaimer's own 'a synchronous walk supersedes the pending ask' rule: `kernel/mm/include/ours/mm/memory_reclaimer.hpp:62` (the second reviewer confirmed only part of it).
- The waiter queue is a third job inside FreeMemoryGate, and its handoff rule lives in free functions: `kernel/mm/pmm.cpp:560` (the second reviewer confirmed only part of it).
- D1(a) (delay level above the zone floors) is enforced at one of the three ways levels get in: `kernel/mm/memory_watchdog.cpp:146`.
- The PMM seam reaches up to the reclaimer singleton, while the watchdog is handed down as an event: `kernel/mm/pmm.cpp:276` (the second reviewer confirmed only part of it).
- Thread start is copied into each service object, and the copies already disagree on errors: `kernel/mm/memory_watchdog.cpp:137` (the second reviewer confirmed only part of it).
- The watchdog's declaration is three fields and a bool, mirrored by an atomic; the pending skip flag is a stray bool: `kernel/mm/include/ours/mm/memory_watchdog.hpp:245`.
- look() asks the levels for bounds and works out the reclaim distance itself, twice: `kernel/mm/memory_watchdog.cpp:203`.
- PmNode works out a zone's distance to High from its getters, while PmZone keeps a never-written BelowHighWmark flag: `kernel/mm/pm_node.cpp:168`.
- AnonymousPageRequest keeps its state machine in a bool, a Gaf and a pointer, and its caller drives it: `kernel/mm/include/ours/mm/page_request.hpp:299`.
- The watchdog unpacks its Bounds into primitives for the gate, which then repeats the 'outside bounds' test twice: `kernel/mm/memory_watchdog.cpp:236`.
- Ending every ordinary wait depends on a running watchdog and on no zone above Normal: `kernel/mm/pmm.cpp:398` (the second reviewer confirmed only part of it).
- alloc_page_table() claims CanBlock while it runs under a spin lock with interrupts off: `kernel/mm/pmm.cpp:489` (the second reviewer confirmed only part of it).
- The unfinished per-CPU cache branch would leak every freed frame if switched on: `kernel/mm/pm_zone.cpp:258`.
- The CPU-to-node table and per-CPU cache are plain memory that move() rewrites while others read: `kernel/mm/include/ours/mm/cpu_node_map.hpp:77`.
- PmNode and PmZone alloc_frames/free_frames are public, so callers can skip the pipeline's checks and its relief wake: `kernel/mm/include/ours/mm/pm_node.hpp:226` (the second reviewer confirmed only part of it).
- create_all() panics the boot on a zone with fewer than two present frames: `kernel/mm/pm_zone.cpp:123`.
- Tests drive PmZone below the seam, which keeps the zone layer public and mixes the layers: `kernel/tests/pm_node_test.cpp:985` (the second reviewer confirmed only part of it).
- One optional sub-check's KTEST_SKIP turns the whole composite MatchesTheMachineItRunsOn into SKIP: `kernel/tests/pm_node_test.cpp:763` (the second reviewer confirmed only part of it).
- WaitingRequestsStopAtLowAndWakeOnRelief claims a waiter is woken, but no thread waits: `kernel/tests/pm_node_test.cpp:1185` (the second reviewer confirmed only part of it).
- Gate state changed by free functions with a manual undo on every exit; the gate is handed a stack Event: `kernel/tests/memory_pressure_test.cpp:244` (the second reviewer confirmed only part of it).
- check_node_queries compares num_nodes() with itself and silently checks nothing on a one-node build: `kernel/tests/pm_node_test.cpp:564`.
- Test oracles decide by hand what the zone already answers, which makes them flaky: `kernel/tests/pm_node_test.cpp:1354`.
- check_disabled_allocation pairs AllocState::disable()/enable() by hand: `kernel/tests/pm_node_test.cpp:803`.
- Admission arithmetic takes swappable same-typed parameters where a Watermarks value type would own them: `kernel/mm/include/ours/mm/priv/admission.hpp:38` (the second reviewer confirmed only part of it).
- Host smoke tests keep their verdict in a file-scope global and duplicate the harness; two comments contradict the code: `kernel/mm/tests/admission_smoke.cpp:23`.
- Thread helpers are duplicated, and a failed resume() would hang the join: `kernel/tests/memory_pressure_test.cpp:322` (the second reviewer confirmed only part of it).
- The fallback-order invariants live only in the test, not in ZoneList: `kernel/tests/pm_node_test.cpp:165` (the second reviewer confirmed only part of it).
- The reserve and the first-pass refusal are checked on the zone, not through the pipeline: `kernel/tests/pm_node_test.cpp:1332`.
- The flat node_* forwarders are a second door to PmNode, and a test exists only to compare the two: `kernel/tests/pm_node_test.cpp:565` (the second reviewer confirmed only part of it).
- Style sweep: static_cast for an enum, const-first, enum layout, dead code after FAIL: `kernel/tests/pm_node_test.cpp:185`.

Refuted: "PmZone is two classes, one zone plus the machine's registry as statics". The zone table inside `PmZone` is the user's decision of 2026-10-29 (`docs/design/261029/zone-table.md`); revisiting it is a question, not a finding.

## Teaching opportunities
### T1. A compare-and-swap on a value that can come back is not a check of what you read
`kernel/mm/pmm.cpp:132` (H1). The invariant the gate protects: "Shut implies the watcher has been signalled". Accessors: every waiting allocation (`admit()`, lock-free), the watcher (`set()`, under `m_watcher_lock`), every release (`check_signal()`). Worst interleaving: CPU A reads `Open`, the level and a low free count; the watcher re-arms with `Open` and new bounds; A's CAS sees `Open` and succeeds, though everything A decided from is stale. This is the ABA problem: CAS compares a value, not a history. Two standard cures: a version counter beside the value in one word (the CAS then fails after any re-arm), or a lock on the rare transition only, keeping the hot path lock-free. Zircon takes the second route with its node lock. See cppreference on `std::atomic::compare_exchange_weak`.

### T2. Single-owner writes do not make cross-owner reads safe
`kernel/mm/include/ours/mm/priv/frame_buddy.hpp:88` (M2). Ownership hand-offs through the zone lock order the writes to a frame, but the buddy reads a frame it does not own, at a moment its owner may be writing it. In the C++ memory model a conflicting non-atomic read and write with no happens-before between them is a data race, undefined however harmless the hardware makes it. The cure is to give the shared fact its own atomic, read relaxed, the way Linux splits `page_type` from the owner's flags.

### T3. Check-then-act across two calls needs one decision, not two looks
`kernel/mm/memory_reclaimer.cpp:121` (M1). `paused()` is looked at, then records are taken, then `paused()` is looked at again in `reclaim()`; a Pause between the first two looks is invisible to both. The general rule: when two actions must agree with one predicate, take the predicate and the action under one lock, or make the action undoable.

### T4. Tell, don't ask
`kernel/mm/pm_node.cpp:110` and `168`. `PmNode` reads `num_free_frames()` and `watermark(High)` from a zone to work out whether it is short and by how much, in two places, while `PmZone::Flags::BelowHighWmark` (`pm_zone.cpp:197`) has no writer but still gates every allocation. Asking a zone `shortfall_to_high()` puts the decision with the data; `free_frames()` returning its crossing (`pm_zone.cpp:248`) is the same idea already done right.

### T5. A passkey instead of a friend
`kernel/mm/include/ours/mm/pm_zone.hpp:328`. `friend class` opens every private member to the friend; a passkey (a type only the intended caller can construct, taken as a parameter) opens one function to one caller. It is how the zone and buddy layers could be sealed against the pipeline being bypassed (M8, M9), without making everything public or everything friendly.

## Suggested follow-up order
1. Naming: `page::Status` shadowing `ours::Status`, `resolve_request`'s `fallback` mask, `suspendable` passed as `interruptible`.
2. H1, the gate's stale CAS, with an interleaving test.
3. M1, the Pause against a pass under way (this redesign's own N2 contract).
4. M2, the buddy's cross-owner read.
5. M4, the request decoding its flags once; then M8 and M9, sealing the node's fallback order and the frame's role.
6. M5 and M6, the frame owning its block layout and the zone owning adoption (F2 as designed).
7. M10, the levels' invariant (changes a documented contract: the user's call).
8. M3, the per-call pipeline object (revisits D1(b)'s wording: the user's call).
9. M7, the placement policy's interface, when the policy is built; its per-allocation lock as a performance item first.
10. M11 and M12, the test trial object and the layout coverage.
