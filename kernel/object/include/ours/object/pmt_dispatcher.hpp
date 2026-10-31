/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PMT_DISPATCHER_HPP
#define OURS_OBJECT_PMT_DISPATCHER_HPP 1

#include <ours/object/bti_dispatcher.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>

#include <span>

namespace ours::object {
/// Rights a freshly created pinned memory token carries: to be looked at, and
/// nothing else. It is not duplicated or sent on, since the last handle to it
/// closing is what the kernel reads as "the driver is gone".
constexpr auto RIGHTS_DEFAULT_PMT = Rights::Inspect;

using PmtDispatcherBase = SoloDispatcher<PmtDispatcher, RIGHTS_DEFAULT_PMT>;

/// The token for one pin (BtiDispatcher::pin()): the driver's hold on memory a
/// device may reach, and the way it says the device is done with it.
///
/// The token is only a handle on the pin; the pin itself belongs to the BTI,
/// which is what lets it live on in the quarantine after the last handle to the
/// token is gone. Until then the token points at the pin, and after an unpin
/// or a quarantine it points at nothing and every question it is asked is
/// BadState.
class PmtDispatcher final: public PmtDispatcherBase {
    using Self = PmtDispatcher;
    using Base = PmtDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Pmt);

    /// Where a stretch of the pinned memory is for the device.
    struct Address {
        /// What to give the device: with no IOMMU to translate, the physical
        /// address.
        u64 address;
        /// The length of the run that starts there, in bytes.
        u64 size;
    };

    /// Where [offset, offset + size) of the pinned range is for the device, as
    /// one run: |offset| is counted from the start of the pin, and |size| is
    /// rounded up to whole pages.
    ///
    /// When the whole range is not one run, the answer is the single page at
    /// |offset| instead, which always is: a caller that wants every address
    /// asks again from the end of each run it is given, as many times as it
    /// takes. |out| is set only on success.
    ///
    ///   - InvArgs for an offset that is not a whole page or an empty range;
    ///   - OutOfRange for a range that is not inside the pin: the token answers
    ///     for what was pinned and nothing else of the VMO;
    ///   - BadState for a token that has been unpinned or quarantined.
    auto query_address(u64 offset, u64 size, Address &out) -> Status;

    /// The addresses of the pieces the pin is cut into, which is how bti_pin
    /// hands them over: piece k is [k * |piece_size|, (k + 1) * |piece_size|)
    /// of the pin, the last one shorter if the pin is not a whole number of
    /// them, and |out[i]| is where piece |first_piece| + i is for the device.
    /// BtiDispatcher::pieces() says how big the pieces are for the options a
    /// pin was made with.
    ///
    /// Each piece has to be one run; a piece that is not is InternalError, since
    /// the options that made the pieces promised it and the pin checked it.
    ///
    ///   - InvArgs for a |piece_size| that is not a whole number of pages;
    ///   - OutOfRange for a piece that starts past the end of the pin;
    ///   - BadState for a token that has been unpinned or quarantined.
    auto query_pieces(u64 piece_size, u64 first_piece, std::span<PhysAddr> out) -> Status;

    /// The driver says its device no longer touches the memory: the pages are
    /// let go. Does nothing more for a token that was unpinned already, so a
    /// path that fails after pinning can call it without asking whether it did.
    auto unpin() -> void;

    /// The last handle is gone. A pin still held is not let go -- the device
    /// may still be writing -- but quarantined by the BTI.
    auto on_zero_handles() -> void override;

  private:
    friend class BtiDispatcher;

    explicit PmtDispatcher(ustl::Arc<BtiDispatcher> bti);
    ~PmtDispatcher() override = default;

    /// Where [offset, offset + size) of |pin| is, as query_address() answers.
    /// The caller has checked that the range lies inside the pin and holds the
    /// token's lock, which is what keeps |pin| alive.
    TA_REQUIRES(get_lock())
    auto run_at_locked(BtiDispatcher::Pin &pin, u64 offset, u64 size, Address &out) -> Status;

    /// Points the token at |pin|, which the BTI has just published. Called once,
    /// before the token is reachable from anywhere else.
    auto attach(BtiDispatcher::Pin &pin) -> void;

    // Set at construction and never changed. Not const: an Arc to a const
    // object cannot call the BTI's own methods.
    ustl::Arc<BtiDispatcher> m_bti;

    // The pin is the BTI's, in one of its lists; this is only a way to it. It
    // is read by every question and cleared by the two ways out, and the lock
    // is what keeps a question from using a pin that an unpin on another
    // handle is freeing: the question holds it across its use.
    BtiDispatcher::Pin *m_pin TA_GUARDED(get_lock()) = nullptr;
};
} // namespace ours::object

#endif // OURS_OBJECT_PMT_DISPATCHER_HPP
