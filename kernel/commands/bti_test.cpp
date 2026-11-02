/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `bti.test`: object::IommuDispatcher, BtiDispatcher and PmtDispatcher, the
// objects a driver gets memory for its device through.
//
// Why a console command: the calls that reach them from a program (iommu_create,
// bti_create, bti_pin, pmt_unpin) come later, and what they promise is about
// the pages' state and the BTI's -- how many are pinned, what is quarantined,
// what a pin refuses -- which no program can read.
//
// On paged VMOs, with a BTI on a stub IOMMU:
//
//   - an IOMMU of a type that is not the stub is NotSupported; a BTI needs one;
//   - every bad argument of a pin is refused, and none leaves a page pinned;
//   - a pin pins the pages it names and no others, and makes a token that is
//     asked where the memory is: one run for the whole range if it is one, or
//     its first page if not, inside the range and nowhere else;
//   - Contiguous asks for one run and is refused for pages that are not;
//   - the addresses of a pin come back cut into pieces, a page each or the whole
//     pin as one, and the pieces and the shape they have are as asked;
//   - an unpin lets the pages go, and answering twice is harmless;
//   - the last handle of a token closed without an unpin quarantines the pin:
//     the pages stay pinned, the BTI refuses new pins, and release_quarantine
//     lets the pages go and the BTI pin again;
//   - a BTI whose last handle is closed refuses new pins for good, and one that
//     is destroyed with a quarantine leaks it: its pages stay pinned, and the
//     records go on holding the VMO;
//   - a pin of a clone at an offset is a pin of the clone's pages, and a pin that
//     fails at its second page leaves its first unpinned.
//
// btistress.test is the same objects from several CPUs at once.
//
// The last check leaks one page, its VMO and a record every time it runs, on
// purpose: that leak is the thing checked.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/mm/gaf.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/object/bti_dispatcher.hpp>
#include <ours/object/iommu_dispatcher.hpp>
#include <ours/object/pmt_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>

#include <array>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize P = PAGE_SIZE;

    constexpr auto RW = BtiPinOptions::PermRead | BtiPinOptions::PermWrite;

    /// |yes| or |no| by |condition|: what a check that depends on where the
    /// frames happen to be expects.
    template <typename T>
    constexpr auto choose(bool const condition, T const yes, T const no) -> T {
        if (condition) {
            return yes;
        }
        return no;
    }

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("bti.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("bti.test: FAIL  {}", name);
    }

    // The analysis cannot tie `node->lock()` to the lock the count requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_pinned(mm::VmObjectPaged &vmo) -> usize {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        return node->nr_pinned_pages_locked();
    }

    auto make_vmo(usize const pages) -> ustl::Arc<mm::VmObjectPaged> {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::GAF_USER, pages * P, mm::VmoFlags::Lazy, &vmo) !=
            Status::Ok) {
            return {};
        }
        return vmo;
    }

    /// Where page |index| of |vmo| is, 0 if it is not there.
    auto phys_of(mm::VmObjectPaged &vmo, usize const index) -> PhysAddr {
        auto pa = PhysAddr{0};
        if (vmo.lookup_contiguous(index * P, P, &pa) != Status::Ok) {
            return 0;
        }
        return pa;
    }

    /// One pin and what it gave: the token, its rights, and the status.
    struct Pinned {
        object::KernelHandle<object::PmtDispatcher> token;
        Rights rights = Rights::None;
        Status status = Status::Ok;
    };

    auto pin(object::BtiDispatcher &bti, ustl::Arc<mm::VmObjectPaged> const &vmo, u64 const offset,
             u64 const size, BtiPinOptions const options) -> Pinned {
        Pinned pinned;
        pinned.status = bti.pin(vmo, offset, size, options, pinned.token, pinned.rights);
        return pinned;
    }

    using PmtAddress = object::PmtDispatcher::Address;

    auto bti_test_command(int, char **) -> int {
        Tally tally;

        // ---- the IOMMU, and the BTI on it
        object::KernelHandle<object::IommuDispatcher> iommu_handle;
        auto iommu_rights = Rights::None;
        verdict(tally,
                object::IommuDispatcher::create(static_cast<IommuType>(7), 0, iommu_handle,
                                                iommu_rights) == Status::NotSupported &&
                    !iommu_handle.dispatcher(),
                "an IOMMU of a type that is not the stub is NotSupported, and makes nothing");
        verdict(tally,
                object::IommuDispatcher::create(IommuType::Stub, 8, iommu_handle, iommu_rights) ==
                        Status::InvArgs &&
                    !iommu_handle.dispatcher(),
                "a stub IOMMU has no hardware to describe, so a descriptor is InvArgs, and makes "
                "nothing");
        verdict(tally,
                object::IommuDispatcher::create(IommuType::Stub, 0, iommu_handle, iommu_rights) ==
                        Status::Ok &&
                    iommu_handle.dispatcher()->iommu_type() == IommuType::Stub &&
                    iommu_rights == object::RIGHTS_DEFAULT_IOMMU && !(iommu_rights & Rights::Wait),
                "a stub IOMMU is made, with the default rights and none to wait");

        object::KernelHandle<object::BtiDispatcher> bti_handle;
        auto bti_rights = Rights::None;
        verdict(tally,
                object::BtiDispatcher::create({}, 1, bti_handle, bti_rights) == Status::InvArgs,
                "a BTI over no IOMMU is InvArgs");
        verdict(tally,
                object::BtiDispatcher::create(iommu_handle.dispatcher(), 0x1234, bti_handle,
                                              bti_rights) == Status::Ok &&
                    bti_handle.dispatcher()->bti_id() == 0x1234 &&
                    bti_handle.dispatcher()->minimum_contiguity() == P &&
                    bti_rights == object::RIGHTS_DEFAULT_BTI && !!(bti_rights & Rights::Map) &&
                    !!(bti_rights & Rights::Write),
                "a BTI is made: its id, one page of contiguity, rights to map and to release");
        if (!bti_handle.dispatcher()) {
            log::error("bti.test: no BTI to go on with");
            return 1;
        }
        auto &bti = *bti_handle.dispatcher();

        auto vmo = make_vmo(4);
        if (!vmo) {
            log::error("bti.test: could not make a VMO");
            return 1;
        }

        // ---- arguments: each refused, none leaves anything pinned
        verdict(tally, pin(bti, vmo, 0, 0, RW).status == Status::InvArgs,
                "an empty range is InvArgs");
        verdict(tally, pin(bti, vmo, 1, P, RW).status == Status::InvArgs,
                "an unaligned offset is InvArgs");
        verdict(tally, pin(bti, vmo, 0, P + 1, RW).status == Status::InvArgs,
                "an unaligned size is InvArgs");
        verdict(tally, pin(bti, vmo, 0, P, BtiPinOptions::None).status == Status::InvArgs,
                "no permission is InvArgs");
        verdict(tally, pin(bti, vmo, 0, P, BtiPinOptions::Compress).status == Status::InvArgs,
                "options with no permission are InvArgs");
        verdict(tally,
                pin(bti, vmo, 0, P, RW | static_cast<BtiPinOptions>(1U << 9)).status ==
                    Status::InvArgs,
                "an option bit that means nothing is InvArgs");
        verdict(
            tally,
            pin(bti, vmo, 0, P, RW | BtiPinOptions::Compress | BtiPinOptions::Contiguous).status ==
                Status::InvArgs,
            "Compress with Contiguous is InvArgs");
        verdict(tally, pin(bti, vmo, 0, 5 * P, RW).status == Status::OutOfRange,
                "a range past the end is OutOfRange");
        verdict(tally, pin(bti, vmo, 4 * P, P, RW).status == Status::OutOfRange,
                "a range that starts at the end is OutOfRange");
        {
            Pinned null_vmo;
            null_vmo.status = bti.pin({}, 0, P, RW, null_vmo.token, null_vmo.rights);
            verdict(tally, null_vmo.status == Status::InvArgs, "no VMO is InvArgs");
        }
        verdict(tally, nr_pinned(*vmo) == 0 && bti.pin_count() == 0,
                "none of them left a page pinned or a pin counted");

        // ---- how many addresses a pin is handed over as
        {
            object::BtiDispatcher::Pieces shape{};
            verdict(tally,
                    bti.pieces(RW, 3 * P, shape) == Status::Ok && shape.size == P &&
                        shape.count == 3,
                    "pieces: a page each by default");
            verdict(tally,
                    bti.pieces(RW | BtiPinOptions::Compress, 3 * P, shape) == Status::Ok &&
                        shape.size == bti.minimum_contiguity() && shape.count == 3,
                    "pieces: a run of the minimum contiguity each with Compress");
            verdict(tally,
                    bti.pieces(RW | BtiPinOptions::Contiguous, 3 * P, shape) == Status::Ok &&
                        shape.size == 3 * P && shape.count == 1,
                    "pieces: the whole pin, once, with Contiguous");
            verdict(tally,
                    bti.pieces(BtiPinOptions::None, P, shape) == Status::InvArgs &&
                        bti.pieces(RW | BtiPinOptions::Compress | BtiPinOptions::Contiguous, P,
                                   shape) == Status::InvArgs &&
                        bti.pieces(RW, 0, shape) == Status::InvArgs &&
                        bti.pieces(RW, P + 1, shape) == Status::InvArgs,
                    "pieces: the options and the size pin() refuses are InvArgs here too");
        }

        // ---- a physical VMO is not pinned
        {
            object::KernelHandle<object::VmoDispatcher> window;
            auto window_rights = Rights::None;
            // Never touched, so it does not matter whether anything answers there.
            auto const made = object::VmoDispatcher::create_physical(
                                  PhysAddr{0xD0000000}, P, window, window_rights) == Status::Ok;
            Pinned pinned;
            auto status = Status::InternalError;
            if (made) {
                status = bti.pin(window.dispatcher()->vmo(), 0, P, RW, pinned.token, pinned.rights);
            }
            verdict(tally, made && status == Status::NotSupported && bti.pin_count() == 0,
                    "a window onto physical memory is NotSupported, and nothing is counted");
        }

        // ---- a pin of two pages
        auto first = pin(bti, vmo, P, 2 * P, RW);
        verdict(tally, first.status == Status::Ok && first.token.dispatcher(),
                "pin(pages 1 and 2) is Ok");
        if (!first.token.dispatcher()) {
            log::error("bti.test: no token to go on with");
            return 1;
        }
        verdict(tally,
                nr_pinned(*vmo) == 2 && bti.pin_count() == 1 && bti.quarantine_count() == 0 &&
                    !bti.in_fault_state() && phys_of(*vmo, 0) == 0 && phys_of(*vmo, 3) == 0,
                "two pages are pinned and one pin is counted; the pages beside are not committed");
        verdict(tally, first.rights == object::RIGHTS_DEFAULT_PMT,
                "the token has the default rights");

        auto &token = *first.token.dispatcher();
        auto const page1 = phys_of(*vmo, 1);
        auto const page2 = phys_of(*vmo, 2);
        auto const adjacent = page1 != 0 && page2 == page1 + P;
        log::info("bti.test: the two pinned pages are {}",
                  choose<char const *>(adjacent, "adjacent", "apart"));

        PmtAddress where{};
        verdict(tally,
                token.query_address(0, 2 * P, where) == Status::Ok && where.address == page1 &&
                    where.size == choose(adjacent, 2 * P, P),
                "the whole range is asked for: its first address, as one run if it is one, "
                "else its first page");
        verdict(tally,
                token.query_address(P, P, where) == Status::Ok && where.address == page2 &&
                    where.size == P,
                "the second page is asked for alone");
        verdict(tally,
                token.query_address(0, 1, where) == Status::Ok && where.address == page1 &&
                    where.size == P,
                "a size is rounded up to whole pages");

        auto before = where;
        verdict(tally,
                token.query_address(0, 0, where) == Status::InvArgs &&
                    token.query_address(1, P, where) == Status::InvArgs,
                "an empty range and an unaligned offset are InvArgs");
        verdict(tally,
                token.query_address(0, 3 * P, where) == Status::OutOfRange &&
                    token.query_address(2 * P, P, where) == Status::OutOfRange &&
                    token.query_address(P, 2 * P, where) == Status::OutOfRange &&
                    token.query_address(~u64{0} & ~(P - 1), P, where) == Status::OutOfRange &&
                    token.query_address(0, ~u64{0}, where) == Status::OutOfRange,
                "a range past the pin is OutOfRange, though the VMO goes on, and so is an "
                "overflowing one");
        verdict(tally, where.address == before.address && where.size == before.size,
                "a failed question leaves its answer as it was");

        // ---- an unpin
        first.token.dispatcher()->unpin();
        verdict(tally,
                nr_pinned(*vmo) == 0 && bti.pin_count() == 0 &&
                    token.query_address(0, P, where) == Status::BadState,
                "an unpin lets the pages go, drops the pin from the count, and the token answers "
                "BadState");
        token.unpin();
        verdict(tally, nr_pinned(*vmo) == 0 && bti.pin_count() == 0,
                "unpinning again does nothing");
        first.token.reset();
        verdict(tally, bti.quarantine_count() == 0 && !bti.in_fault_state(),
                "closing the token after an unpin quarantines nothing");

        // ---- options that are accepted
        for (auto const options: {BtiPinOptions::PermRead, BtiPinOptions::PermWrite,
                                  BtiPinOptions::PermExecute | BtiPinOptions::Compress,
                                  RW | BtiPinOptions::Contiguous}) {
            auto pinned = pin(bti, vmo, 0, P, options);
            verdict(tally, pinned.status == Status::Ok && nr_pinned(*vmo) == 1,
                    "one page is pinned with this combination of options");
            if (pinned.token.dispatcher()) {
                pinned.token.dispatcher()->unpin();
            }
        }
        verdict(tally, nr_pinned(*vmo) == 0, "and each unpin let it go");

        // ---- contiguous or not, by where the frames are
        //
        // Two layouts of two pages. Committed one after the other in a VMO of
        // their own, the pages are most likely adjacent; committed in turn
        // with another VMO's, most likely not. Neither is guaranteed, so what
        // is asked of each is what its frames say: one run exactly when the
        // second is the frame after the first.
        auto fill = make_vmo(2);
        auto apart = make_vmo(2);
        auto together = make_vmo(2);
        auto const committed =
            fill && apart && together &&
            apart->commit_range(0, P, mm::VmObject::CommitOptions::None) == Status::Ok &&
            fill->commit_range(0, P, mm::VmObject::CommitOptions::None) == Status::Ok &&
            apart->commit_range(P, P, mm::VmObject::CommitOptions::None) == Status::Ok &&
            fill->commit_range(P, P, mm::VmObject::CommitOptions::None) == Status::Ok &&
            together->commit_range(0, 2 * P, mm::VmObject::CommitOptions::None) == Status::Ok;
        verdict(tally, committed, "two VMOs of two pages are committed, in two orders");
        if (committed) {
            for (auto layout: {apart, together}) {
                auto const first_frame = phys_of(*layout, 0);
                auto const one_run = first_frame != 0 && phys_of(*layout, 1) == first_frame + P;
                log::info("bti.test: a layout of two pages is {}",
                          choose<char const *>(one_run, "one run", "apart"));

                auto contiguous = pin(bti, layout, 0, 2 * P, RW | BtiPinOptions::Contiguous);
                verdict(tally,
                        contiguous.status == choose(one_run, Status::Ok, Status::BadState) &&
                            nr_pinned(*layout) == choose<usize>(one_run, 2, 0),
                        "Contiguous is Ok for one run and BadState for pages apart, and a refusal "
                        "leaves nothing pinned");
                if (contiguous.token.dispatcher()) {
                    contiguous.token.dispatcher()->unpin();
                }

                auto plain = pin(bti, layout, 0, 2 * P, RW);
                PmtAddress run{};
                verdict(tally,
                        plain.status == Status::Ok && plain.token.dispatcher() &&
                            plain.token.dispatcher()->query_address(0, 2 * P, run) == Status::Ok &&
                            run.address == first_frame && run.size == choose(one_run, 2 * P, P),
                        "without Contiguous the same pages are pinned, and asked for as the layout "
                        "allows");
                if (plain.token.dispatcher()) {
                    // The same pin, cut into pieces as bti_pin hands it over.
                    auto &pinned_token = *plain.token.dispatcher();
                    auto const second_frame = phys_of(*layout, 1);
                    std::array<PhysAddr, 2> by_page{};
                    std::array<PhysAddr, 1> one{};
                    verdict(tally,
                            pinned_token.query_pieces(P, 0, by_page) == Status::Ok &&
                                by_page[0] == first_frame && by_page[1] == second_frame,
                            "in pieces of a page, each page's address");
                    verdict(tally,
                            pinned_token.query_pieces(P, 1, one) == Status::Ok &&
                                one[0] == second_frame,
                            "a later piece alone");
                    one[0] = 0;
                    verdict(tally,
                            pinned_token.query_pieces(2 * P, 0, one) ==
                                    choose(one_run, Status::Ok, Status::InternalError) &&
                                one[0] == choose<PhysAddr>(one_run, first_frame, 0),
                            "the whole pin as one piece is Ok for one run and InternalError for "
                            "pages apart");
                    verdict(tally,
                            pinned_token.query_pieces(P, 2, one) == Status::OutOfRange &&
                                pinned_token.query_pieces(P, ~u64{0}, one) == Status::OutOfRange &&
                                pinned_token.query_pieces(0, 0, one) == Status::InvArgs &&
                                pinned_token.query_pieces(P + 1, 0, one) == Status::InvArgs,
                            "a piece past the pin is OutOfRange, though the VMO goes on, and a "
                            "piece that is not whole pages is InvArgs");
                    pinned_token.unpin();
                    verdict(tally, pinned_token.query_pieces(P, 0, by_page) == Status::BadState,
                            "an unpinned token answers BadState to pieces too");
                }
                verdict(tally, nr_pinned(*layout) == 0 && bti.pin_count() == 0,
                        "and unpinned again");
            }
        }

        // ---- a token closed with no unpin: the quarantine
        {
            auto lost = pin(bti, vmo, 3 * P, P, RW);
            verdict(tally, lost.status == Status::Ok && nr_pinned(*vmo) == 1, "a page is pinned");
            // The token outlives its handle here, to be asked afterwards.
            auto lost_token = lost.token.dispatcher();
            lost.token.reset();
            verdict(tally,
                    nr_pinned(*vmo) == 1 && bti.quarantine_count() == 1 && bti.pin_count() == 1 &&
                        bti.in_fault_state(),
                    "closing the last handle of the token with no unpin quarantines the pin: the "
                    "page stays pinned and the BTI is in a fault state");

            PmtAddress asked{};
            verdict(tally, lost_token->query_address(0, P, asked) == Status::BadState,
                    "a quarantined token answers BadState, as an unpinned one does");

            auto refused = pin(bti, vmo, 0, P, RW);
            verdict(tally,
                    refused.status == Status::BadState && nr_pinned(*vmo) == 1 &&
                        bti.pin_count() == 1,
                    "a BTI in a fault state pins nothing new, and a refusal leaves nothing pinned");
            auto untouched = make_vmo(1);
            verdict(tally,
                    untouched && pin(bti, untouched, 0, P, RW).status == Status::BadState &&
                        phys_of(*untouched, 0) == 0,
                    "and it refuses before committing the pages it would have pinned");

            bti.release_quarantine();
            verdict(tally,
                    nr_pinned(*vmo) == 0 && bti.quarantine_count() == 0 && bti.pin_count() == 0 &&
                        !bti.in_fault_state(),
                    "release_quarantine lets the page go, and the BTI is out of its fault state");
            bti.release_quarantine();
            verdict(tally, nr_pinned(*vmo) == 0 && bti.pin_count() == 0,
                    "releasing an empty quarantine does nothing");

            auto again = pin(bti, vmo, 0, P, RW);
            verdict(tally, again.status == Status::Ok && nr_pinned(*vmo) == 1,
                    "and the BTI pins again");
            if (again.token.dispatcher()) {
                again.token.dispatcher()->unpin();
            }

            // More than one in the quarantine, all let go by one release.
            auto lost_a = pin(bti, vmo, 0, P, RW);
            auto lost_b = pin(bti, vmo, P, 2 * P, RW);
            auto lost_c = pin(bti, vmo, 3 * P, P, RW);
            lost_a.token.reset();
            lost_b.token.reset();
            lost_c.token.reset();
            verdict(tally,
                    lost_a.status == Status::Ok && lost_b.status == Status::Ok &&
                        lost_c.status == Status::Ok && bti.quarantine_count() == 3 &&
                        nr_pinned(*vmo) == 4,
                    "three tokens closed with no unpin put three pins in the quarantine");
            bti.release_quarantine();
            verdict(tally,
                    bti.quarantine_count() == 0 && bti.pin_count() == 0 && nr_pinned(*vmo) == 0,
                    "one release_quarantine lets all three go");

            // A pin still held is not touched by a release, only the quarantine.
            auto held = pin(bti, vmo, 0, P, RW);
            auto lost_one = pin(bti, vmo, P, P, RW);
            lost_one.token.reset();
            bti.release_quarantine();
            verdict(tally,
                    held.status == Status::Ok && lost_one.status == Status::Ok &&
                        nr_pinned(*vmo) == 1 && bti.pin_count() == 1,
                    "release_quarantine leaves a pin that has a token pinned");
            if (held.token.dispatcher()) {
                held.token.dispatcher()->unpin();
            }
        }

        // ---- a clone: a window at an offset, whose pages are its own once pinned
        {
            auto parent = make_vmo(4);
            ustl::Arc<mm::VmObjectPaged> child;
            auto const made =
                parent &&
                parent->commit_range(0, 4 * P, mm::VmObject::CommitOptions::None) == Status::Ok &&
                parent->create_clone(P, 2 * P, &child) == Status::Ok && child;
            verdict(tally, made,
                    "a VMO of four pages is committed and a clone of two made at page 1");
            if (made) {
                auto slice = pin(bti, child, 0, 2 * P, RW);
                PmtAddress at{};
                verdict(tally,
                        slice.status == Status::Ok && slice.token.dispatcher() &&
                            nr_pinned(*child) == 2 && nr_pinned(*parent) == 0 &&
                            slice.token.dispatcher()->query_address(P, P, at) == Status::Ok &&
                            at.address == phys_of(*child, 1) && at.size == P,
                        "the clone's pages are pinned and asked for through the clone, and the "
                        "parent's are not pinned");
                if (slice.token.dispatcher()) {
                    slice.token.dispatcher()->unpin();
                }
                verdict(tally, nr_pinned(*child) == 0 && bti.pin_count() == 0,
                        "and unpinned again");
            }
        }

        // ---- a pin that fails part way
        {
            auto crowded = make_vmo(2);
            auto saturated = true;
            for (usize count = 0; crowded && count < 255; ++count) {
                saturated = saturated && crowded->pin_range(P, P) == Status::Ok;
            }
            verdict(tally, crowded && saturated && nr_pinned(*crowded) == 1,
                    "page 1 of a VMO is pinned as many times as a page can be");
            if (crowded) {
                auto failed = pin(bti, crowded, 0, 2 * P, RW);
                verdict(tally,
                        failed.status == Status::NoResource && nr_pinned(*crowded) == 1 &&
                            bti.pin_count() == 0 && !failed.token.dispatcher(),
                        "a range that cannot be pinned whole is NoResource and leaves page 0 "
                        "unpinned too");
                auto released = true;
                for (usize count = 0; count < 255; ++count) {
                    released = released && crowded->unpin_range(P, P) == Status::Ok;
                }
                verdict(tally, released && nr_pinned(*crowded) == 0, "and the 255 are undone");
            }
        }

        // ---- a BTI whose last handle is closed
        {
            object::KernelHandle<object::BtiDispatcher> orphan_handle;
            auto orphan_rights = Rights::None;
            auto const made =
                object::BtiDispatcher::create(iommu_handle.dispatcher(), 2, orphan_handle,
                                              orphan_rights) == Status::Ok;
            ustl::Arc<object::BtiDispatcher> orphan;
            if (made) {
                orphan = orphan_handle.dispatcher();
            }
            auto page = make_vmo(1);
            verdict(tally, made && page, "a second BTI and a VMO are made");
            if (made && page) {
                auto kept = pin(*orphan, page, 0, P, RW);
                auto lost = pin(*orphan, page, 0, P, RW);
                auto freed = pin(*orphan, page, 0, P, RW);
                verdict(tally,
                        kept.status == Status::Ok && lost.status == Status::Ok &&
                            freed.status == Status::Ok && nr_pinned(*page) == 1 &&
                            orphan->pin_count() == 3,
                        "a page is pinned three times, by three tokens");

                lost.token.reset();
                orphan_handle.reset();
                verdict(tally, orphan->quarantine_count() == 1 && orphan->pin_count() == 3,
                        "the last handle of the BTI is closed with a pin quarantined and two held");

                auto refused = pin(*orphan, page, 0, P, RW);
                verdict(tally, refused.status == Status::BadState,
                        "a BTI whose last handle is closed pins nothing new");

                freed.token.dispatcher()->unpin();
                freed.token.reset();
                verdict(tally,
                        orphan->pin_count() == 2 && nr_pinned(*page) == 1 &&
                            orphan->quarantine_count() == 1,
                        "a pin made before is unpinned all the same, and the quarantine is "
                        "still there");

                kept.token.reset();
                verdict(tally,
                        orphan->pin_count() == 2 && orphan->quarantine_count() == 2 &&
                            nr_pinned(*page) == 1,
                        "a token closed with no unpin after the BTI's own is quarantined too");

                // The destructor leaks what is quarantined: nobody can release it
                // now, and its device may still be writing. The page stays pinned,
                // and the VMO we hold with it.
                auto const holders = page.use_count();
                orphan = {};
                verdict(tally, nr_pinned(*page) == 1 && page.use_count() == holders,
                        "the BTI is destroyed with a quarantine: its page stays pinned, and the "
                        "records go on holding the VMO");
            }
        }

        if (tally.failed != 0) {
            log::error("bti.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("bti.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(BtiTest, "bti.test", ours::bti_test_command,
             "check the IOMMU, BTI and PMT objects: pins, tokens, the quarantine and a BTI "
             "that is closed");
