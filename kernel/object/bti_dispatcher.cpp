/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/object/bti_dispatcher.hpp>
#include <ours/object/pmt_dispatcher.hpp>

#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::object {
BtiDispatcher::BtiDispatcher(ustl::Arc<IommuDispatcher> iommu, u64 const bti_id)
    : Base(), m_iommu(ustl::move(iommu)), m_bti_id(bti_id) {}

BtiDispatcher::~BtiDispatcher() {
    // A pin in the active list has a token that points at it, and a token
    // holds this object: it cannot be gone while one is there.
    ASSERT(m_active.empty(), "a BTI is destroyed with a pin that has a token");

    // What is left in the quarantine is leaked on purpose. The last handle is
    // gone, so no driver can say its device has stopped, and the device may
    // still write: the pages stay pinned and the VMO stays alive, for good,
    // rather than be handed to someone else with a device writing into them.
    // The pins were logged as they were lost (quarantine(), on_zero_handles()).
    while (auto front = m_quarantine.front()) {
        auto &pin = *front;
        auto _ = m_quarantine.erase(m_quarantine.iterator_to(pin));
        [[maybe_unused]]
        auto &leaked = ustl::Box<Pin>::from_raw(&pin).leak();
    }
}

auto BtiDispatcher::create(ustl::Arc<IommuDispatcher> iommu, u64 const bti_id,
                           KernelHandle<Self> &handle_out, Rights &rights_out) -> Status {
    if (!iommu) {
        return Status::InvArgs;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::ALLOC_KERNEL) Self(ustl::move(iommu), bti_id));
    if (!self) {
        return Status::NoMemory;
    }

    rights_out = default_right();
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto BtiDispatcher::check_options(BtiPinOptions const options) -> Status {
    constexpr auto PERMS =
        BtiPinOptions::PermRead | BtiPinOptions::PermWrite | BtiPinOptions::PermExecute;
    constexpr auto KNOWN = PERMS | BtiPinOptions::Compress | BtiPinOptions::Contiguous;
    if (!!(options & ~KNOWN) || !(options & PERMS)) {
        return Status::InvArgs;
    }
    if (!!(options & BtiPinOptions::Compress) && !!(options & BtiPinOptions::Contiguous)) {
        return Status::InvArgs;
    }
    return Status::Ok;
}

auto BtiDispatcher::pieces(BtiPinOptions const options, u64 const size, Pieces &out) const
    -> Status {
    if (auto const status = check_options(options); status != Status::Ok) {
        return status;
    }
    if (size == 0 || !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    auto piece = u64{PAGE_SIZE};
    if (!!(options & BtiPinOptions::Compress)) {
        piece = minimum_contiguity();
    } else if (!!(options & BtiPinOptions::Contiguous)) {
        piece = size;
    }
    // Rounded up: the last piece is what is left, and a pin that is not a
    // whole number of pieces has one more.
    auto count = size / piece;
    if (size % piece != 0) {
        count += 1;
    }
    out = {.size = piece, .count = count};
    return Status::Ok;
}

auto BtiDispatcher::unpin_and_free(ustl::Box<Pin> pin) -> void {
    auto const status = pin->vmo->unpin_range(pin->offset, pin->size);
    if (status != Status::Ok) {
        // The record is freed all the same and its pages stay as they are, so
        // this is the one trace of a pin that was not what it said.
        log::error("bti: a pin of [{:#x}, {:#x}) was not pinned in its VMO: {}", pin->offset,
                   pin->offset + pin->size, to_string(status));
    }
    DEBUG_ASSERT(status == Status::Ok, "a pin of a BTI was not pinned in its VMO");
}

auto BtiDispatcher::pin(ustl::Arc<mm::VmObject> vmo, u64 const offset, u64 const size,
                        BtiPinOptions const options, KernelHandle<PmtDispatcher> &pmt_out,
                        Rights &rights_out) -> Status {
    m_canary.verify<Self>();

    if (!vmo) {
        return Status::InvArgs;
    }
    if (auto const status = check_options(options); status != Status::Ok) {
        return status;
    }
    if (size == 0 || !ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    // A BTI that will refuse is asked before the pages are committed and
    // pinned for nothing. Only advice: the BTI can change before the pin is
    // published, which is why the check under the lock below stays.
    if (!accepts_pins()) {
        return Status::BadState;
    }

    // The pages first: from here to the end the range is held, and a failure
    // on the way must give it back, or a pin would be left that no token names.
    if (auto const status = vmo->pin_range(offset, size); status != Status::Ok) {
        return status;
    }
    auto const fail = [&](Status const status) {
        [[maybe_unused]]
        auto const undone = vmo->unpin_range(offset, size);
        DEBUG_ASSERT(undone == Status::Ok, "a range just pinned could not be unpinned");
        return status;
    };

    if (!!(options & BtiPinOptions::Contiguous)) {
        // No translation stands between the device and memory, so the range is
        // contiguous for it only if it is in memory.
        auto first = PhysAddr{0};
        if (auto const status = vmo->lookup_contiguous(offset, size, &first);
            status != Status::Ok) {
            return fail(status);
        }
    }

    // Both objects before the lock: the lock is held for the check and the
    // publication, and nothing that can fail or wait is allowed inside it.
    auto *const raw_record = new (mm::ALLOC_KERNEL) Pin(vmo, offset, size);
    if (raw_record == nullptr) {
        return fail(Status::NoMemory);
    }
    auto record = ustl::Box<Pin>::from_raw(raw_record);
    auto token = ustl::Arc<PmtDispatcher>::adopt(new (mm::ALLOC_KERNEL)
                                                     PmtDispatcher(ustl::Arc<Self>::adopt(this)));
    if (!token) {
        return fail(Status::NoMemory);
    }

    auto refused = false;
    {
        Guard guard{get_lock()};
        refused = !!(m_flags & Flags::Orphaned) || !m_quarantine.empty();
        if (!refused) {
            m_active.push_back(*record.into_raw());
        }
    }
    if (refused) {
        // Not published, and the token points at nothing, so dropping it
        // quarantines nothing.
        return fail(Status::BadState);
    }
    token->attach(*raw_record);

    pmt_out = KernelHandle<PmtDispatcher>(ustl::move(token));
    rights_out = PmtDispatcher::default_right();
    return Status::Ok;
}

auto BtiDispatcher::accepts_pins() -> bool {
    Guard guard{get_lock()};
    return !(m_flags & Flags::Orphaned) && m_quarantine.empty();
}

auto BtiDispatcher::release_active(Pin &pin) -> void {
    {
        Guard guard{get_lock()};
        m_active.erase(pin);
    }
    unpin_and_free(ustl::Box<Pin>::from_raw(&pin));
}

auto BtiDispatcher::quarantine(Pin &pin) -> void {
    usize count = 0;
    usize pages = 0;
    bool orphaned = false;
    {
        Guard guard{get_lock()};
        m_active.erase(pin);
        m_quarantined_pages += pin.size / PAGE_SIZE;
        m_quarantine.push_back(pin);
        count = m_quarantine.size();
        pages = m_quarantined_pages;
        orphaned = !!(m_flags & Flags::Orphaned);
    }

    if (orphaned) {
        log::warn("bti {} (id {:#x}): the last handle of a pin was closed without an unpin, after "
                  "the BTI's own; {} pin(s), {} page(s) are quarantined and can no longer be "
                  "released: leaked",
                  koid(), m_bti_id, count, pages);
        return;
    }
    log::warn("bti {} (id {:#x}): the last handle of a pin was closed without an unpin; {} pin(s), "
              "{} page(s) are quarantined until release_quarantine",
              koid(), m_bti_id, count, pages);
}

auto BtiDispatcher::release_quarantine() -> void {
    m_canary.verify<Self>();

    // Taken whole, as one step: a pin quarantined while this runs is a pin
    // whose device the caller has not vouched for, and must stay.
    Pin::List released;
    {
        Guard guard{get_lock()};
        released.swap(m_quarantine);
        m_quarantined_pages = 0;
    }

    while (auto front = released.front()) {
        auto &pin = *front;
        auto _ = released.erase(released.iterator_to(pin));
        unpin_and_free(ustl::Box<Pin>::from_raw(&pin));
    }
}

auto BtiDispatcher::pin_count() -> usize {
    Guard guard{get_lock()};
    return m_active.size() + m_quarantine.size();
}

auto BtiDispatcher::quarantine_count() -> usize {
    Guard guard{get_lock()};
    return m_quarantine.size();
}

auto BtiDispatcher::on_zero_handles() -> void {
    usize count = 0;
    usize pages = 0;
    {
        Guard guard{get_lock()};
        m_flags |= Flags::Orphaned;
        count = m_quarantine.size();
        pages = m_quarantined_pages;
    }

    if (count != 0) {
        log::warn("bti {} (id {:#x}): the last handle was closed with {} pin(s), {} page(s) "
                  "quarantined; they can no longer be released: leaked",
                  koid(), m_bti_id, count, pages);
    }
}
} // namespace ours::object
