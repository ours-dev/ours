# kernel-irq outstanding repairs

## 1. An empty line accepts a registration that combines Permanent and Shared

**Severity**
Medium: the registration violates the settled policy contract and becomes permanently attached, although Permanent currently prevents actual sharing and no in-tree caller requests this combination on an empty line.

**Symptom**
Latent, cannot be triggered by today's production callers. A kernel caller constructing an Action with IrqFlags::Permanent | IrqFlags::Shared and attaching it to an empty line receives Ok, and the resulting registration cannot be detached. The contract says Permanent cannot be Shared. Existing irq.test coverage uses this combination only on an occupied line, where the occupancy check rejects it for another reason.

**Cause**
`Desc::check_oneshot()` (`kernel/kernel/irq/desc.cpp:206`) returns Ok immediately for every action without Oneshot. `Desc::attach()` checks Permanent against occupancy, but accepts it when the line is empty. `Desc::link_locked()` (`:235`) then derives both Shared and Permanent from the action. Registration policy validation is incorrectly scoped to Oneshot, even though incompatible policy exists independently of deferred handling.

**Change**
Validate registration policy as a whole before publishing any registration, for both attach overloads. Reject Permanent with Shared as InvArgs, as Permanent with Oneshot already is. Add empty-line tests for both attach forms, asserting refusal, an unattached action, and unchanged line state; keep valid Permanent and Shared cases. A larger naming or domain-ownership rewrite is not a dependency.

**Reference**
This project's own Permanent-line policy: `.agents/skills/kernel-design/references/task.md`, Interrupt lines, and `.agents/tasks/irq-multi-arch/DESIGN.md`, D4b. Zircon's `InterruptTableEntry::set_handler()` in `zircon/kernel/platform/pc/interrupt_manager.rs` publishes one immutable permanent handler; it has no shared-action policy. Linux's `__setup_irq()` in `kernel/irq/manage.c` owns registration-policy validation, but ours's Permanent option is not a Linux flag.

**Acceptance**
Both attach overloads refuse Permanent | Shared on an empty line with InvArgs, without linking the action or changing the line's flow or controller configuration. irq.test passes valid Permanent, exclusive and shared registration checks. Both images and host tests pass `python3 .agents/skills/verify/scripts/boot_check.py --out out/<lane>`, and the relevant kernel suites pass on that output before the fix is committed.
