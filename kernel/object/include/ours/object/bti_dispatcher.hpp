/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_BTI_DISPATCHER_HPP
#define OURS_OBJECT_BTI_DISPATCHER_HPP 1

#include <ours/bti.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/iommu_dispatcher.hpp>
#include <ours/object/types.hpp>

#include <ustl/box.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::object {
/// Rights a freshly created BTI handle carries: Map to pin memory for the
/// device, Write to release the quarantine. Not waited on.
constexpr auto RIGHTS_DEFAULT_BTI = (Rights::Basic & ~Rights::Wait) | Rights::Io | Rights::Map;

using BtiDispatcherBase = SoloDispatcher<BtiDispatcher, RIGHTS_DEFAULT_BTI>;

/// The bus transaction initiator: one device's way to memory, and the record of
/// what memory has been put within its reach.
///
/// A driver that lets its device read or write memory by DMA has to know where
/// the memory is, and that it stays there while the device may touch it. Both
/// come from one call, pin(): it holds the pages of a VMO range in their
/// frames and answers with a token, a PmtDispatcher, whose addresses are what
/// the device is given. The memory is the driver's own VMO; what the BTI adds
/// is that the kernel knows which of it a device can reach, and decides when it
/// may be moved or freed again.
///
/// A pin ends in one of two ways, and the difference is the point of the
/// object:
///
///   - The driver unpins it (PmtDispatcher::unpin()), having made sure its
///     device no longer uses the memory: the pages are let go.
///   - The last handle to the token is closed first, as when the driver dies.
///     The kernel cannot tell whether the device is still writing, and with no
///     IOMMU it cannot stop it, so the pages stay pinned in the quarantine, out
///     of the page allocator and out of the reclaimer. The BTI is then in a
///     fault state and pins nothing new, until a driver that has reset its
///     device says so with release_quarantine().
///
/// A BTI that loses its last handle with a quarantine left can never be
/// released, so its quarantine is leaked: some memory is lost, in exchange for
/// no device ever writing into a page that has been handed to someone else.
/// Both are logged.
class BtiDispatcher final: public BtiDispatcherBase {
    using Self = BtiDispatcher;
    using Base = BtiDispatcherBase;

    /// What one pin is: the range, and the VMO that holds its pages pinned.
    /// Owned by the BTI, which is what lets it outlive the token (a pin in the
    /// quarantine has no token any more), and found by the token through
    /// PmtDispatcher::m_pin.
    struct Pin {
        using Self = Pin;

        Pin(ustl::Arc<mm::VmObject> pinned_vmo, u64 start, u64 length)
            : vmo(ustl::move(pinned_vmo)), offset(start), size(length) {}

        ustl::Arc<mm::VmObject> vmo;
        u64 offset;
        u64 size;

        ustl::collections::intrusive::ListMemberHook<> hook;
        USTL_DECLARE_HOOK_OPTION(Self, hook, Option);
        USTL_DECLARE_LIST(Self, List, Option);
    };
    friend class PmtDispatcher;

    enum class Flags : u32 {
        None = 0,
        /// The last handle is gone: nobody can release the quarantine any more,
        /// and a pin made now would never be let go by a driver.
        Orphaned = 1U << 0,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Bti);

    /// A BTI on |iommu| for the device |bti_id| names. What the id means is the
    /// IOMMU's: the stub has no devices to tell apart and takes any.
    static auto create(ustl::Arc<IommuDispatcher> iommu, u64 bti_id, KernelHandle<Self> &handle_out,
                       Rights &rights_out) -> Status;

    /// Pins [offset, offset + size) of |vmo| for the device, and makes the
    /// token for it.
    ///
    ///   - InvArgs for an offset or size that is not whole pages, an empty
    ///     range, an option bit that means nothing, no Perm bit, or Compress
    ///     with Contiguous;
    ///   - OutOfRange past the end of the VMO; NotSupported for a VMO that is
    ///     not memory of the kernel's own (a window onto a device); NoResource
    ///     for a page already pinned as many times as it can be;
    ///   - BadState for a BTI that has lost its last handle or is in a fault
    ///     state, and for Contiguous over memory that is not one run;
    ///   - NoMemory.
    ///
    /// Whether the caller may give the device this memory with these
    /// permissions is the rights on its handle to |vmo|, which the system call
    /// checks before it gets here: this layer asks the Perm bits for nothing
    /// but that one is set.
    auto pin(ustl::Arc<mm::VmObject> vmo, u64 offset, u64 size, BtiPinOptions options,
             KernelHandle<PmtDispatcher> &pmt_out, Rights &rights_out) -> Status;

    /// Lets go of every pin in the quarantine: the pages may move and be freed
    /// again, which is for a driver that has stopped its device to say.
    auto release_quarantine() -> void;

    /// The longest run of device addresses that is certain to come back as one
    /// when the memory is contiguous: a page for this IOMMU, which can hand out
    /// nothing smaller or promise nothing larger.
    [[nodiscard]]
    auto minimum_contiguity() const -> u64 {
        return PAGE_SIZE;
    }

    [[nodiscard]]
    auto bti_id() const -> u64 {
        return m_bti_id;
    }

    /// Pins held now, active and quarantined; a snapshot.
    [[nodiscard]]
    auto pin_count() -> usize;

    /// Pins in the quarantine now; a snapshot.
    [[nodiscard]]
    auto quarantine_count() -> usize;

    /// Whether the BTI refuses new pins because of a quarantine.
    [[nodiscard]]
    auto in_fault_state() -> bool {
        return quarantine_count() != 0;
    }

    /// The last handle is gone: no more pins, and a quarantine left is lost.
    auto on_zero_handles() -> void override;

  private:
    BtiDispatcher(ustl::Arc<IommuDispatcher> iommu, u64 bti_id);
    ~BtiDispatcher() override;

    /// Whether a pin made now would be published: the last handle is not gone
    /// and nothing is quarantined. A snapshot.
    auto accepts_pins() -> bool;

    /// Called by the token. The pin is in the active list and the token has
    /// just stopped pointing at it.
    ///
    /// release_active() is an unpin by the driver; quarantine() is the last
    /// handle of the token closing without one.
    auto release_active(Pin &pin) -> void;
    auto quarantine(Pin &pin) -> void;

    /// Undoes the pin and frees the record. No lock is held: the VMO's own
    /// lock is taken, and the BTI's lock is not allowed to be held across it.
    static auto unpin_and_free(ustl::Box<Pin> pin) -> void;

    ustl::Arc<IommuDispatcher> const m_iommu;
    u64 const m_bti_id;

    // Both lists, the page count of the second, and the flags are one state
    // that changes together: a pin moves between the lists, or a pin is
    // refused, as one step. One lock for them is what makes the check in pin()
    // ("no quarantine, not orphaned") and the publication of the new pin a
    // single step, so a pin cannot be made into a BTI that has just stopped
    // taking them. Nothing taken under this lock is a lock of the VMO.
    Pin::List m_active TA_GUARDED(get_lock());
    Pin::List m_quarantine TA_GUARDED(get_lock());
    usize m_quarantined_pages TA_GUARDED(get_lock()) = 0;
    Flags m_flags TA_GUARDED(get_lock()) = Flags::None;
};
} // namespace ours::object

#endif // OURS_OBJECT_BTI_DISPATCHER_HPP
