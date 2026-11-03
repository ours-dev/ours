/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_MOD_HPP
#define OURS_IRQ_MOD_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

#include <ktl/result.hpp>
#include <ustl/option.hpp>

namespace arch {
struct IrqFrame;
}

namespace ours::irq {
class Registration;

/// Controls borrow line storage inside an IRQ RCU scope. Writes take the
/// line Guard, L1, then any controller register lock. Callbacks run under L1
/// with interrupts and preemption disabled. A handler must never call line
/// control, including control of a different line; no ordering exists between
/// two line locks. Callers must not hold a lock that a line callback takes.
/// No control sleeps or allocates.
///
/// Ordinary controls return InvArgs for IrqId::Invalid, NoResource for an
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
auto attach(IrqId id, Registration &registration) -> Status;

/// Exclusively acquire and configure/read the mode under one L1 hold. With
/// mode, configure it; with None, query it. Leave the source masked until its
/// owner unmasks. Occupancy is checked before hardware changes. The returned
/// mode remains stable while the registration is attached.
[[nodiscard]]
auto attach(IrqId id, Registration &registration, ustl::Option<LineMode> mode)
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
auto finalize_oneshot(IrqId id, Registration const &registration) -> void;

/// Serialize a controller mask with dispatch. A retiring line may be masked.
[[nodiscard]]
auto mask(IrqId id) -> Status;

/// Start delivery unless Held, in which case return Ok without reopening.
/// A retiring line returns NoResource.
[[nodiscard]]
auto unmask(IrqId id) -> Status;

/// Configure an unoccupied line and leave it masked. Occupied lines return
/// AlreadyBound; a retiring line returns NoResource. Acquire a particular
/// mode through attach(mode) when configuration and ownership must be atomic.
[[nodiscard]]
auto configure(IrqId id, TriggerMode trigger, Polarity polarity) -> Status;

/// Read hardware mode under the controller's own lock, without L1. Safe in a
/// callback; a caller needing stable ownership uses attach(mode).
[[nodiscard]]
auto trigger_mode(IrqId id) -> ktl::Result<TriggerMode>;

/// Resolve a domain-local source. Invalid means no mapping. Kernel identities
/// are never reused, so the answer may be retained after the lookup scope.
[[nodiscard]]
auto irq_for_source(DomainId domain, HardwareIrq source) -> IrqId;

/// Recover the controller-local source of a known kernel identity. Invalid
/// means no mapping. The caller supplies its domain when resolving it again.
[[nodiscard]]
auto source_for_irq(IrqId id) -> HardwareIrq;

/// Permanent occupancy is advisory for user error selection; attach remains
/// the authority that excludes a conflicting owner.
[[nodiscard]]
auto is_kernel_owned(IrqId id) -> bool;

/// Unclaimed deliveries since boot, or None for an absent line.
[[nodiscard]]
auto spurious_count(IrqId id) -> ustl::Option<u64>;

/// Test seams retain the same reader scope and line guards as production.
/// Synthetic delivery skips controller ACK/EOI; no interrupt is in service.
[[nodiscard]]
auto handle_irq_for_test(IrqId id) -> IrqReturn;
[[nodiscard]]
auto is_masked_for_test(IrqId id) -> ktl::Result<bool>;
[[nodiscard]]
auto root_source_exists_for_test(HardwareIrq source) -> bool;
[[nodiscard]]
auto revoke_line_for_test(IrqId id) -> Status;
auto unmark_dying_for_test(IrqId id) -> void;

/// The bookkeeping on entry to a trap, which arch calls once, before it
/// dispatches: records the frame and raises the counts that keep this CPU from
/// switching threads until the exit. Exceptions take it too, a page fault
/// among them.
auto handle_irq_preamble(::arch::IrqFrame *frame) -> void;

/// The bookkeeping on exit from a trap, which arch calls once, after the
/// dispatch. Answers whether the CPU should switch threads before returning
/// to the interrupted context; the switch is arch's.
auto handle_irq_postamble() -> bool;

/// The function the root interrupt controller's driver gives arch. Arch calls
/// it for an interrupt that comes through the root controller, after it has
/// separated the causes it can tell apart without the controller (x86's
/// vector, a RISC-V trap cause: the local timer and the inter-processor
/// interrupts) and before the postamble. Where only the controller's own read
/// tells them apart (an acknowledge read returns the processor-local
/// interrupts too), the handler hands those back to arch. The handler finds
/// out which interrupt it is in the way its controller allows (a vector, an
/// acknowledge read, a claim), hands it to DomainEndpoint::dispatch(), and ends the
/// interrupt itself if no line had it, since nothing else knows how.
using RootHandler = auto (*)(::arch::IrqFrame *frame) -> void;

/// Installs `handler` as the root handler. There is one for the kernel, and it
/// is installed once, before the first interrupt of the root controller can be
/// taken: the controller's driver does it when it starts, ahead of the
/// instruction that enables interrupts. A second call is a kernel bug.
auto set_root_handler(RootHandler handler) -> void;

/// Calls the root handler, in interrupt context. An interrupt that arrives
/// before one is installed is a kernel bug.
auto handle_root_irq(::arch::IrqFrame *frame) -> void;

/// Trap frame of the interrupt currently handled on this CPU, or nullptr
/// outside interrupt context.
auto current_frame() -> ::arch::IrqFrame *;
} // namespace ours::irq

#endif // OURS_IRQ_MOD_HPP
