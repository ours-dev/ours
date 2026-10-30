/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `pagereclaim.test`: the pages a page source supplied are given back, the
// oldest first, and asked of the source again when they are needed
// (.agents/tasks/page-reclaim, step 4).
//
//   - PageQueues::reclaim() evicts the oldest reclaimable pages first, and no
//     more than it was asked for;
//   - an evicted page is a hole: the next access raises a request against the
//     page source, and a page supplied again reads as it did;
//   - a page written is not reclaimable, and stays with what was written;
//   - a page of a page source is mapped read-only even in a writable mapping,
//     so that a write comes back to the node and is seen; an evicted page is
//     taken out of the mappings;
//   - a node destroyed while the reclaimer looks at its pages is neither lost
//     nor used after it is freed: many VMOs made and dropped on one thread
//     while another reclaims.
//
// Why a console command: the queues are the real nodes', filled by the real
// VMO paths, and the stress needs threads.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/memmap.hpp>
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
#include <ours/task/thread.hpp>

#include <atomic>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
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
            log::info("pagereclaim.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pagereclaim.test: FAIL  {}", name);
    }

    /// A provider nobody answers, which counts the requests made of it: the
    /// test supplies the pages itself.
    class CountingProvider final: public mm::PageProvider {
      public:
        [[nodiscard]]
        auto nr_sent() const -> usize {
            return m_sent;
        }

      private:
        auto send_async_request(mm::PageRequest *) -> void override {
            m_sent += 1;
        }
        auto clear_async_request(mm::PageRequest *) -> void override {}
        auto swap_async_request(mm::PageRequest *, mm::PageRequest *) -> void override {}
        auto wait_on_event(sync::Event *, bool) -> Status override {
            return Status::TimeOut;
        }
        auto on_detach() -> void override {}
        auto on_close() -> void override {}

        usize m_sent = 0;
    };

    /// What a page is supplied with: its own index, so that a page that comes
    /// back at the wrong offset, or not at all, shows.
    auto pattern(u64 const index) -> u64 {
        return 0x9a9e'50c0'0000'0000 | index;
    }

    /// A VMO of |nr_pages| pages over a page source whose provider is
    /// |provider|, nothing supplied yet.
    auto make_vmo(usize const nr_pages, CountingProvider **const provider)
        -> ustl::Arc<mm::VmObjectPaged> {
        auto *const made = new (mm::GAF_KERNEL) CountingProvider();
        if (!made) {
            return {};
        }
        *provider = made;
        ustl::Arc<mm::PageSource> source;
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::PageSource::create(ustl::Arc<mm::PageProvider>::adopt(made), &source) !=
                Status::Ok ||
            mm::VmObjectPaged::with_source(source, mm::GAF_USER, nr_pages * P, mm::VmoFlags::Lazy,
                                           &vmo) != Status::Ok) {
            return {};
        }
        return vmo;
    }

    /// Supplies |count| pages from page |first| of |vmo|, each filled with its
    /// pattern.
    auto supply(mm::VmObjectPaged &vmo, usize const first, usize const count) -> bool {
        mm::VmPageSpliceList pages(count);
        if (pages.reserve(count) != Status::Ok) {
            return false;
        }
        for (usize i = 0; i < count; ++i) {
            auto *const frame = mm::alloc_frames(mm::GAF_KERNEL | mm::Gaf::Zero);
            if (!frame) {
                return false;
            }
            *reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt(mm::frame_to_phys(frame))) =
                pattern(first + i);
            if (pages.append_page(i, frame) != Status::Ok) {
                mm::free_frames(frame, 0);
                return false;
            }
        }
        return vmo.supply_pages(first * P, count * P, &pages,
                                mm::VmObject::SupplyOption::Default) == Status::Ok;
    }

    /// The page |vmo| holds at page |index|, and what kind of queue it is on.
    struct Filed {
        mm::VmPage *page = nullptr;
        Kind kind = Kind::None;
    };

    // The analysis cannot tie `node->lock()` to the lock the cursor requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto filed_at(mm::VmObjectPaged &vmo, usize const index) -> Filed {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(index * P, P);
        if (!cursor) {
            return {};
        }
        // Looking is not using.
        cursor->disable_mark_accessed();
        auto const found = cursor->maybe_page();
        if (found.is_none()) {
            return {};
        }
        auto *const page = found.value().page;
        return {.page = page, .kind = mm::PmNode::node(page->nid())->page_queues().kind_of(*page)};
    }

    auto queues_of(mm::VmPage const &page) -> mm::PageQueues & {
        return mm::PmNode::node(page.nid())->page_queues();
    }

    auto word_at(mm::VmObjectPaged &vmo, usize const index, u64 *const out) -> bool {
        return vmo.read(out, index * P, sizeof(*out)) == Status::Ok;
    }

    /// Every reclaimable page on every node.
    auto nr_reclaimable() -> usize {
        usize total = 0;
        mm::PmNode::for_each_node(mm::PmNode::State::Online, [&total](mm::PmNode &node) {
            total += node.page_queues().count(Kind::Reclaimable);
        });
        return total;
    }

    /// Whether a read of page |index| of |vmo| is asked of its page source,
    /// and not answered: the page is a hole.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto is_asked_for(mm::VmObjectPaged &vmo, usize const index) -> bool {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(index * P, P);
        mm::MultiPageRequest request;
        auto const read = cursor->require_page(false, 1, &request);
        return read.is_err() && read.unwrap_err() == Status::ShouldWait;
    }

    auto check_order_and_refetch(Tally &tally) -> void {
        CountingProvider *provider = nullptr;
        auto vmo = make_vmo(4, &provider);
        auto const baseline = nr_reclaimable();
        // Two pages, a rotation of every node, two more: ages 1, 1, 0, 0.
        auto const supplied = vmo && supply(*vmo, 0, 2);
        mm::PmNode::for_each_node(mm::PmNode::State::Online,
                                  [](mm::PmNode &node) { node.page_queues().rotate(); });
        auto const supplied_more = supplied && supply(*vmo, 2, 2);
        if (!supplied_more) {
            verdict(tally, false, "a VMO of four supplied pages is made");
            return;
        }
        auto const first = filed_at(*vmo, 0);
        if (!first.page) {
            verdict(tally, false, "the first page is there");
            return;
        }
        auto &queues = queues_of(*first.page);
        verdict(tally, nr_reclaimable() == baseline + 4,
                "the four pages are reclaimable, and nothing else is on the node's queues");

        auto const freed = queues.reclaim(2);
        verdict(tally,
                freed == 2 && !filed_at(*vmo, 0).page && !filed_at(*vmo, 1).page &&
                    filed_at(*vmo, 2).page && filed_at(*vmo, 3).page,
                "reclaim(2) evicts the two oldest pages and no others");

        auto const asked_before = provider->nr_sent();
        auto const evicted_asked = is_asked_for(*vmo, 0);
        auto const asked_after = provider->nr_sent();
        // A page that is there is read, not asked for.
        auto const kept_asked = is_asked_for(*vmo, 2);
        verdict(tally,
                evicted_asked && asked_after == asked_before + 1 && !kept_asked &&
                    provider->nr_sent() == asked_after,
                "an evicted page is asked of its page source again, a page kept is not");

        u64 word = 0;
        auto const again = supply(*vmo, 0, 1) && word_at(*vmo, 0, &word);
        verdict(tally, again && word == pattern(0) && filed_at(*vmo, 0).kind == Kind::Reclaimable,
                "supplied again, an evicted page reads as before and is reclaimable");

        // Page 3 written: it is the VMO's own now, not the source's.
        u64 const mine = 0x0dd5'ca1e'0dd5'ca1e;
        auto const wrote = vmo->write(&mine, 3 * P, sizeof(mine)) == Status::Ok;
        auto const dirty = filed_at(*vmo, 3);
        verdict(tally, wrote && dirty.kind == Kind::Anonymous,
                "a page written leaves the reclaimable queues");

        auto const rest = queues.reclaim(100);
        u64 back = 0;
        verdict(tally,
                rest == 2 && !filed_at(*vmo, 0).page && !filed_at(*vmo, 1).page &&
                    !filed_at(*vmo, 2).page && filed_at(*vmo, 3).page == dirty.page &&
                    word_at(*vmo, 3, &back) && back == mine,
                "reclaim(100) evicts every page but the written one, which keeps what was written");
    }

    auto check_mappings(Tally &tally) -> void {
        CountingProvider *provider = nullptr;
        auto vmo = make_vmo(2, &provider);
        ustl::Arc<mm::VmAspace> aspace;
        if (!vmo || !supply(*vmo, 0, 2) ||
            mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                                 mm::VmasFlags::None, "pagereclaim.test", &aspace) != Status::Ok) {
            verdict(tally, false, "a VMO and a user address space are made");
            return;
        }
        auto root = aspace->root_vma();
        auto mapping = root->map_with_vmo(
            mm::VmPlacement::anywhere(), 2 * P, 0,
            mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User,
            mm::VmaFlags::PermMask, vmo, mm::VmMapOption::Populate, "pagereclaim.test");
        auto const page = filed_at(*vmo, 1).page;
        if (mapping.is_err() || !page) {
            verdict(tally, false, "the VMO is mapped");
        } else {
            auto const base = mapping.unwrap()->base();
            PhysAddr pa = 0;
            mm::MmuFlags flags{};
            auto const mapped = aspace->arch_aspace().query(base + P, &pa, &flags) == Status::Ok;
            verdict(tally,
                    mapped && pa == mm::frame_to_phys(page) && !(flags & mm::MmuFlags::Writable),
                    "a page of a page source is mapped read-only, though the mapping allows write");

            auto const freed = queues_of(*page).reclaim(2);
            auto const gone = aspace->arch_aspace().query(base + P, &pa, &flags);
            verdict(tally, freed == 2 && gone == Status::NotFound,
                    "an evicted page is taken out of the mapping");
        }
        if (aspace->destroy() != Status::Ok) {
            verdict(tally, false, "the address space is destroyed");
        }
    }

    struct Stress {
        std::atomic<bool> done{false};
        std::atomic<usize> made{0};
    };

    /// Makes a VMO of supplied pages and drops it, many times.
    auto stress_routine(void *const arg) -> i32 {
        auto &stress = *static_cast<Stress *>(arg);
        for (usize round = 0; round < 400; ++round) {
            CountingProvider *provider = nullptr;
            auto vmo = make_vmo(4, &provider);
            if (!vmo || !supply(*vmo, 0, 4)) {
                break;
            }
            stress.made.fetch_add(1, std::memory_order_relaxed);
            // |vmo| goes here, while the other thread may be looking at its
            // pages.
        }
        stress.done.store(true, std::memory_order_release);
        return 0;
    }

    auto check_lifetime(Tally &tally) -> void {
        Stress stress;
        auto const before = nr_reclaimable();
        auto *const worker =
            task::Thread::spawn("reclaim-stress", stress_routine, &stress, task::DEFAULT_PRIORITY);
        if (!worker || worker->resume() != Status::Ok) {
            verdict(tally, false, "the thread that makes and drops VMOs is started");
            return;
        }
        usize freed = 0;
        while (!stress.done.load(std::memory_order_acquire)) {
            mm::PmNode::for_each_node(mm::PmNode::State::Online, [&freed](mm::PmNode &node) {
                freed += node.page_queues().reclaim(3);
            });
            task::Thread::Current::yield();
        }
        i32 code = 0;
        auto const joined = worker->join(&code, time::Deadline::infinite()) == Status::Ok;
        log::info("pagereclaim.test:   {} VMOs made, {} pages evicted while they were dropped",
                  stress.made.load(), freed);
        verdict(tally,
                joined && code == 0 && stress.made.load() == 400 && nr_reclaimable() == before,
                "VMOs dropped while a reclaimer looks at their pages leave nothing behind");
    }

    auto pagereclaim_test_command(int, char **) -> int {
        Tally tally;
        check_order_and_refetch(tally);
        check_mappings(tally);
        check_lifetime(tally);

        if (tally.failed != 0) {
            log::error("pagereclaim.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pagereclaim.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(
    PageReclaimTest, "pagereclaim.test", ours::pagereclaim_test_command,
    "check that pages a page source supplied are given back oldest first, and asked again");
