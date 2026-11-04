/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `pagequeues.test`: every page a VMO holds is filed on its node's page queue
// for its kind, with a backlink to the node that holds it, and taken off when
// it leaves (.agents/tasks/page-reclaim, step 1).
//
//   - a page written into a hole of an anonymous VMO is anonymous zero-fork,
//     owned by the VMO's node at its index;
//   - cloned, the page moves to the hidden node and keeps its kind; written
//     by the clone, the clone's copy is anonymous and the clone's; written by
//     the original, which is then its last viewer, the page is the original's
//     again;
//   - a pinned VMO's pages are pinned;
//   - pages a page source supplied are reclaimable, owned by the VMO's node;
//   - once every VMO here is gone, every queue holds what it held before.
//
// And that they age (step 2):
//
//   - the node's aging thread rotates the ring within a period;
//   - a supplied page is in the newest generation, two rotations make it two
//     generations old, and a read through the VMO makes it the newest again;
//   - mapped into a user address space, a page read only through the mapping
//     is a generation younger after a rotation than one not read: the harvest
//     of the accessed bit (step 3), and its shootdown, without which the read
//     hits the TLB and sets no bit.
//
// Why a console command: the queues are the real nodes', filled by the real
// VMO paths.
#include <ours/arch/aspace_layout.hpp>
#include <ours/boot/options.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using Kind = mm::PageQueues::PageKind;
    constexpr u64 P = PAGE_SIZE;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("pagequeues.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pagequeues.test: FAIL  {}", name);
    }

    /// The page |vmo| sees at |offset|, and what its descriptor says about it.
    struct Filed {
        mm::VmPage *page = nullptr;
        Kind kind = Kind::None;
        void *owner = nullptr;
        PgOff index = 0;
    };

    // The analysis cannot tie `node->lock()` to the lock the cursor requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end, and the descriptor is read under it, as the queues require.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto filed_at(mm::VmObjectPaged &vmo, u64 const offset) -> Filed {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(offset, P);
        // Looking is not using: the checks read a page's age after this.
        cursor.disable_mark_accessed();
        auto const found = cursor.maybe_page();
        if (found.is_none()) {
            return {};
        }
        auto *const page = found.value().page;
        return {.page = page,
                .kind = mm::PmNode::node(page->nid())->page_queues().kind_of(*page),
                .owner = page->get_object(),
                .index = page->get_pgoff()};
    }

    /// The node |vmo| sees its pages through, to compare a backlink with.
    auto node_of(mm::VmObjectPaged &vmo) -> void const * {
        return vmo.cow_pages().as_ptr();
    }

    /// Every queue's count on node 0, the one node of the default build.
    auto counts() -> std::array<usize, 5> {
        auto &queues = mm::PmNode::node(0)->page_queues();
        return {queues.count(Kind::None), queues.count(Kind::Pinned), queues.count(Kind::Anonymous),
                queues.count(Kind::AnonymousZeroFork), queues.count(Kind::Reclaimable)};
    }

    auto write_word(mm::VmObjectPaged &vmo, u64 const offset, u64 const value) -> bool {
        return vmo.write(&value, offset, sizeof(value)) == Status::Ok;
    }

    auto check_anonymous(Tally &tally) -> void {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::ALLOC_USER_VMO, 4 * P, mm::VmoFlags::Lazy, &vmo) !=
            Status::Ok) {
            verdict(tally, false, "an anonymous VMO is made");
            return;
        }
        auto const written = write_word(*vmo, P, 0x0123'4567'89ab'cdef);
        auto const first = filed_at(*vmo, P);
        verdict(tally,
                written && first.page && first.kind == Kind::AnonymousZeroFork &&
                    first.owner == node_of(*vmo) && first.index == 1,
                "a page written into a hole is anonymous zero-fork, the VMO's, at its index");

        ustl::Arc<mm::VmObjectPaged> clone;
        auto const cloned = vmo->create_clone(0, 4 * P, &clone) == Status::Ok;
        auto const hidden = filed_at(*vmo, P);
        verdict(tally,
                cloned && hidden.page == first.page && hidden.kind == Kind::AnonymousZeroFork &&
                    hidden.owner != node_of(*vmo) && hidden.owner != nullptr && hidden.index == 1,
                "cloned, the page moves to the hidden node and keeps its kind");

        auto const clone_wrote = cloned && write_word(*clone, P, 0x5eed);
        auto const copy = filed_at(*clone, P);
        verdict(tally,
                clone_wrote && copy.page && copy.page != first.page &&
                    copy.kind == Kind::Anonymous && copy.owner == node_of(*clone) &&
                    copy.index == 1,
                "the clone's copy of the page is anonymous and the clone's");

        auto const original_wrote = write_word(*vmo, P, 0xfeed);
        auto const taken = filed_at(*vmo, P);
        verdict(tally,
                original_wrote && taken.page == first.page &&
                    taken.kind == Kind::AnonymousZeroFork && taken.owner == node_of(*vmo) &&
                    taken.index == 1,
                "the last viewer takes the hidden node's page, which is its own again");
    }

    auto check_pinned(Tally &tally) -> void {
        ustl::Arc<mm::VmObjectPaged> vmo;
        auto const made = mm::VmObjectPaged::create(mm::ALLOC_KERNEL, 2 * P, mm::VmoFlags::Pinned,
                                                    &vmo) == Status::Ok;
        auto page = Filed{};
        if (made) {
            page = filed_at(*vmo, P);
        }
        verdict(tally,
                made && page.page && page.kind == Kind::Pinned && page.owner == node_of(*vmo) &&
                    page.index == 1,
                "a pinned VMO's pages are pinned");
    }

    /// A provider nobody asks: the test supplies the pages itself.
    class SilentProvider final: public mm::PageProvider {
        auto send_async_request(mm::PageRequest *) -> void override {}
        auto clear_async_request(mm::PageRequest *) -> void override {}
        auto swap_async_request(mm::PageRequest *, mm::PageRequest *) -> void override {}
        auto wait_on_event(sync::Event *, bool) -> Status override {
            return Status::TimeOut;
        }
        auto on_detach() -> void override {}
        auto on_close() -> void override {}
    };

    /// A VMO of two pages whose content comes from a page source nobody asks,
    /// both supplied; empty if any of that failed.
    auto supplied_vmo() -> ustl::Arc<mm::VmObjectPaged> {
        auto *const provider = new (mm::ALLOC_KERNEL) SilentProvider();
        if (!provider) {
            return {};
        }
        ustl::Arc<mm::PageSource> source;
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::PageSource::create(ustl::Arc<mm::PageProvider>::adopt(provider), &source) !=
                Status::Ok ||
            mm::VmObjectPaged::create_external(source, mm::ALLOC_USER, 2 * P, mm::VmoFlags::Lazy,
                                               &vmo) != Status::Ok) {
            return {};
        }

        mm::VmPageSpliceList pages(2);
        auto supplied = pages.reserve(2) == Status::Ok;
        for (usize i = 0; supplied && i < 2; ++i) {
            auto *const frame =
                mm::alloc_frames(mm::ALLOC_KERNEL | mm::Gaf::Zero).value_or(nullptr);
            supplied = frame && pages.append_page(i, frame) == Status::Ok;
        }
        if (!supplied || vmo->supply_pages(0, 2 * P, &pages, mm::VmObject::SupplyOption::Default) !=
                             Status::Ok) {
            return {};
        }
        return vmo;
    }

    auto check_reclaimable(Tally &tally) -> void {
        auto vmo = supplied_vmo();
        auto page = Filed{};
        if (vmo) {
            page = filed_at(*vmo, P);
        }
        verdict(tally,
                vmo && page.page && page.kind == Kind::Reclaimable && page.owner == node_of(*vmo) &&
                    page.index == 1,
                "a page a page source supplied is reclaimable, the VMO's, at its index");
    }

    auto check_aging_thread(Tally &tally) -> void {
        // The next rotation is due at most a period after the last one, so
        // waiting a period and a second sees one, whenever the last one was.
        auto const &queues = mm::PmNode::node(0)->page_queues();
        auto const before = queues.rotations();
        auto const period = Seconds(boot::options().page_aging_seconds());
        auto _ = task::Thread::Current::sleep_for(period + Seconds(1), task::Interruptible::No);
        verdict(tally, queues.rotations() != before, "the aging thread rotates within a period");
    }

    auto check_aging(Tally &tally) -> void {
        // Every node rotated first, before the pages are supplied: that puts
        // each aging thread's next rotation a period away, so none of its own
        // lands between the checks below, whichever node the pages come from.
        mm::PmNode::for_each_node(mm::PmNode::State::Online,
                                  [](mm::PmNode &node) { node.page_queues().rotate(); });

        auto vmo = supplied_vmo();
        auto page = Filed{};
        if (vmo) {
            page = filed_at(*vmo, P);
        }
        if (!page.page || page.kind != Kind::Reclaimable) {
            verdict(tally, false, "a supplied page to age is made");
            return;
        }
        auto &queues = mm::PmNode::node(page.page->nid())->page_queues();
        verdict(tally, queues.age_of(*page.page) == 0,
                "a supplied page is in the newest generation");

        queues.rotate();
        queues.rotate();
        verdict(tally, queues.age_of(*page.page) == 2, "two rotations make it two generations old");

        u64 word = 0;
        auto const read = vmo->read(&word, P, sizeof(word)) == Status::Ok;
        verdict(tally, read && queues.age_of(*page.page) == 0,
                "a read through the VMO makes it the newest again");
    }

    /// Puts the calling thread on |next| and loads its page tables, and hands
    /// back what it was on before, with preemption off across the two steps,
    /// as usercopy.test does and for its reason: the scheduler loads the
    /// tables of the thread's address space on every switch.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    /// Every node rotated, each harvesting first: the pages may be on any.
    auto rotate_every_node() -> void {
        mm::PmNode::for_each_node(mm::PmNode::State::Online,
                                  [](mm::PmNode &node) { node.page_queues().rotate(); });
    }

    auto age_of(Filed const &filed) -> usize {
        return mm::PmNode::node(filed.page->nid())->page_queues().age_of(*filed.page);
    }

    /// Both pages of a supplied VMO mapped, present, into an address space of
    /// the test's own; the second read through the mapping. A read of a
    /// present page takes no fault and so no lookup, and no software mark:
    /// only the hardware's accessed bit records it.
    auto check_harvest(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        auto vmo = supplied_vmo();
        if (!vmo ||
            mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                                 mm::VmasFlags::None, "pagequeues.test", &aspace) != Status::Ok) {
            verdict(tally, false, "a user address space and a supplied VMO are made");
            return;
        }
        auto *const previous = attach(aspace.as_ptr_mut());
        auto root = aspace->root_vma();
        auto mapping = root->map_with_vmo(
            VirtAddr::Anywhere, 2 * P, 0, mm::MmuFlags::Readable | mm::MmuFlags::User,
            mm::VmaFlags::PermMask, vmo, mm::VmMapOption::Populate, "pagequeues.test");
        auto const first = filed_at(*vmo, 0);
        auto const second = filed_at(*vmo, P);
        if (mapping.is_err() || !first.page || !second.page) {
            verdict(tally, false, "the VMO is mapped");
        } else {
            auto const base = mapping.unwrap()->base();
            u64 word = 0;
            // Read once before the first harvest too, so that this CPU's TLB
            // holds the translation the harvest's shootdown has to drop.
            auto const loaded = user_copy::copy_from_user(&word, base + P, sizeof(word));
            // Both bits are set, as every leaf is made: both pages to the
            // newest generation, then a generation older.
            rotate_every_node();
            auto const read = user_copy::copy_from_user(&word, base + P, sizeof(word));
            rotate_every_node();
            log::info("pagequeues.test:   ages after a read through the mapping: {} and {}",
                      age_of(first), age_of(second));
            verdict(tally,
                    loaded == Status::Ok && read == Status::Ok && age_of(first) == 2 &&
                        age_of(second) == 1,
                    "a page read only through a mapping is a generation younger than one not");
        }
        attach(previous);
        if (aspace->destroy() != Status::Ok) {
            verdict(tally, false, "the address space is destroyed");
        }
    }

    auto pagequeues_test_command(int, char **) -> int {
        Tally tally;
        auto const before = counts();
        check_anonymous(tally);
        check_pinned(tally);
        check_reclaimable(tally);
        check_aging_thread(tally);
        check_aging(tally);
        check_harvest(tally);
        auto const after = counts();
        verdict(tally, before == after,
                "with every VMO here gone, every queue holds what it held before");
        if (before != after) {
            log::error("pagequeues.test: pinned {} -> {}, anonymous {} -> {}, zero-fork {} -> {}, "
                       "reclaimable {} -> {}",
                       before[1], after[1], before[2], after[2], before[3], after[3], before[4],
                       after[4]);
        }

        if (tally.failed != 0) {
            log::error("pagequeues.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pagequeues.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PageQueuesTest, "pagequeues.test", ours::pagequeues_test_command,
             "check that VMO pages are filed on their node's page queues by kind, and taken off");
