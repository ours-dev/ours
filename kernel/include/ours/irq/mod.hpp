/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_MOD_HPP
#define OURS_IRQ_MOD_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ktl/result.hpp>
#include <ustl/option.hpp>

namespace ours::irq {
class Registration;

/// Controls borrow line storage inside an IRQ RCU scope. Writes take the
/// line Guard, L1, then any controller register lock. Callbacks run under L1
/// with interrupts and preemption disabled. A handler must never call line
/// control, including control of a different line; no ordering exists between
/// two line locks. Callers must not hold a lock that a line callback takes.
/// No control sleeps or allocates.
///
/// Ordinary controls return InvArgs for VIrqNum::Invalid, NoResource for an
/// absent/withdrawn line, and NotSupported for a missing controller operation.

/// Attach caller-owned, address-stable registration storage. The owner
/// serializes attach/detach of that registration and retains it and its
/// context until detach returns. Shared registrations may join only a Shared
/// line; Permanent takes an empty line for the kernel's lifetime. Oneshot
/// cannot share or be Permanent, and requires bounded readiness plus a
/// synchronous mask/unmask contract. Invalid policy is rejected before any
/// line or hardware change. BadState means already attached; AlreadyBound
/// means conflicting occupancy. Without a mode, Oneshot needs a readable mode.
[[nodiscard]]
auto attach(VIrqNum id, Registration &registration) -> Status;

/// Exclusively acquire and configure/read the mode under one L1 hold. With
/// mode, configure it; with None, query it. Leave the source masked until its
/// owner unmasks. Occupancy is checked before hardware changes. The returned
/// mode remains stable while the registration is attached.
[[nodiscard]]
auto attach(VIrqNum id, Registration &registration, ustl::Option<LineMode> mode)
    -> ktl::Result<TriggerMode>;

/// Drain callbacks and unlink this registration. Last detach masks the source
/// and clears its Shared/Oneshot/Held state. Permanent returns AlreadyBound;
/// already detached or revoked returns NotFound. The owner separately drains
/// its own completion callers before reusing storage or freeing context.
[[nodiscard]]
auto detach(Registration &registration) -> Status;

/// Complete deferred work on this identity and registration. Under L1, check
/// attachment and revalidate the owner's current readiness before clearing
/// Held and unmasking. No-op after detach, for a different registration, for
/// non-Oneshot or while newer work is pending. It never repeats ACK or EOI.
/// An explicit identity remains valid while detach clears the binding.
auto finalize_oneshot(VIrqNum id, Registration const &registration) -> void;

/// Serialize a controller mask with dispatch. A retiring line may be masked.
[[nodiscard]]
auto mask(VIrqNum id) -> Status;

/// Start delivery unless Held, in which case return Ok without reopening.
/// A retiring line returns NoResource.
[[nodiscard]]
auto unmask(VIrqNum id) -> Status;

/// Configure an unoccupied line and leave it masked. Occupied lines return
/// AlreadyBound; a retiring line returns NoResource. Acquire a particular
/// mode through attach(mode) when configuration and ownership must be atomic.
[[nodiscard]]
auto configure(VIrqNum id, TriggerMode trigger, Polarity polarity) -> Status;

/// Read hardware mode under the controller's own lock, without L1. Safe in a
/// callback; a caller needing stable ownership uses attach(mode).
[[nodiscard]]
auto trigger_mode(VIrqNum id) -> ktl::Result<TriggerMode>;

/// Resolve a domain-local source. Invalid means no mapping. Kernel identities
/// are never reused, so the answer may be retained after the lookup scope.
[[nodiscard]]
auto irq_for_source(DomainId domain, HIrqNum source) -> VIrqNum;

/// Recover the controller-local source of a known kernel identity. Invalid
/// means no mapping. The caller supplies its domain when resolving it again.
[[nodiscard]]
auto source_for_irq(VIrqNum id) -> HIrqNum;

/// Permanent occupancy is advisory for user error selection; attach remains
/// the authority that excludes a conflicting owner.
[[nodiscard]]
auto is_kernel_owned(VIrqNum id) -> bool;

/// Unclaimed deliveries since boot, or None for an absent line.
[[nodiscard]]
auto spurious_count(VIrqNum id) -> ustl::Option<u64>;

/// Test seams retain the same reader scope and line guards as production.
/// Synthetic delivery skips controller ACK/EOI; no interrupt is in service.
[[nodiscard]]
auto handle_irq_for_test(VIrqNum id) -> IrqReturn;
[[nodiscard]]
auto is_masked_for_test(VIrqNum id) -> ktl::Result<bool>;
[[nodiscard]]
auto root_source_exists_for_test(HIrqNum source) -> bool;
[[nodiscard]]
auto revoke_line_for_test(VIrqNum id) -> Status;
auto unmark_dying_for_test(VIrqNum id) -> void;

} // namespace ours::irq

#endif // OURS_IRQ_MOD_HPP
