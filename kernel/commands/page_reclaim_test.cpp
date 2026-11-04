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
//   - a page pinned and unpinned is written as far as the node knows: the device
//     that held it may have changed it, so it is not reclaimable either, and
//     keeps what the device wrote;
//   - a page of a page source is mapped read-only even in a writable mapping,
//     so that a write comes back to the node and is seen; an evicted page is
//     taken out of the mappings;
//   - a node destroyed while the reclaimer looks at its pages is neither lost
//     nor used after it is freed: many VMOs made and dropped on one thread
//     while another reclaims;
//
// and (step 5) the anonymous pages that are all zero are given back:
//
//   - a page committed by a write of zeros becomes a zero marker and is freed,
//     reading the same; a page that holds something is left, and is not looked
//     at again; a write to a marker commits a new page;
//   - a zero page is taken out of the mappings that show it;
//   - a program writing a page through a mapping while it is deduplicated
//     never loses the write: pages flip between zero and not under a writer
//     on one thread while another deduplicates;
//
// and (step 6) the memory watchdog has them given back:
//
//   - at Critical the reclaimer gives back about what is asked, zero pages
//     before any pager page; with more asked than there are zero pages, the
//     pager pages go too;
//   - at OutOfMemory the watchdog has them given back on its own thread before
//     it refuses the allocations that could wait, and does not refuse them.
//
// Why a console command: the queues are the real nodes', filled by the real
// VMO paths, and the stress needs threads.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/memory_watchdog.hpp>
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

#include <atomic>
#include <string_view>

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
        auto *const made = new (mm::ALLOC_KERNEL) CountingProvider();
        if (!made) {
            return {};
        }
        *provider = made;
        ustl::Arc<mm::PageSource> source;
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::PageSource::create(ustl::Arc<mm::PageProvider>::adopt(made), &source) !=
                Status::Ok ||
            mm::VmObjectPaged::with_source(source, mm::ALLOC_USER, nr_pages * P, mm::VmoFlags::Lazy,
                                           &vmo) != Status::Ok) {
            return {};
        }
        return vmo;
    }

    /// Supplies |count| pages from page |first| of |vmo|, each filled with its
    /// pattern.
    auto supply(mm::VmObjectPaged &vmo, usize const first, usize const count,
                NodeId const nid = NodeId{MAX_NODES}) -> bool {
        mm::VmPageSpliceList pages(count);
        if (pages.reserve(count) != Status::Ok) {
            return false;
        }
        auto flags = mm::ALLOC_KERNEL | mm::Gaf::Zero;
        if (nid < MAX_NODES) {
            flags |= mm::Gaf::ThisNode;
        }
        for (usize i = 0; i < count; ++i) {
            auto *const frame = mm::alloc_frames(nid, flags, 0).value_or(nullptr);
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
            VirtAddr::Anywhere, 2 * P, 0,
            mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User,
            mm::VmaFlags::PermMask, vmo, mm::VmMapOption::Populate, "pagereclaim.test");
        auto const page = filed_at(*vmo, 1).page;
        if (mapping.is_err() || !page) {
            verdict(tally, false, "the VMO is mapped");
        } else {
            auto const base = mapping.unwrap()->base();
            PhysAddr pa{};
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

    /// A pin hands a page of a page source to a device that may write it, and
    /// pin_range() takes the page through require_owned_page(), which marks it
    /// written. The unpin has to keep that mark: filed as the source's content
    /// again, the page would be evicted and what the device wrote lost.
    auto check_unpin_keeps_the_write(Tally &tally) -> void {
        CountingProvider *provider = nullptr;
        auto vmo = make_vmo(2, &provider);
        if (!vmo || !supply(*vmo, 0, 2)) {
            verdict(tally, false, "a VMO of two supplied pages is made");
            return;
        }
        auto const pinned = vmo->pin_range(0, P) == Status::Ok;
        auto const page = filed_at(*vmo, 0).page;
        verdict(tally,
                pinned && page && filed_at(*vmo, 0).kind == Kind::Pinned &&
                    filed_at(*vmo, 1).kind == Kind::Reclaimable,
                "a page pinned is on the pinned queue, the page beside it stays reclaimable");
        if (!page) {
            return;
        }

        // The device writes the page while it holds the pin.
        u64 const device = 0xde71'ce00'dead'beef;
        *reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt(mm::frame_to_phys(page))) = device;

        auto const unpinned = vmo->unpin_range(0, P) == Status::Ok;
        verdict(tally, unpinned && filed_at(*vmo, 0).kind == Kind::Anonymous,
                "a page unpinned is written, not reclaimable");

        auto const freed = queues_of(*page).reclaim(100);
        u64 back = 0;
        verdict(tally,
                freed == 1 && filed_at(*vmo, 0).page == page && !filed_at(*vmo, 1).page &&
                    word_at(*vmo, 0, &back) && back == device,
                "reclaim(100) evicts the page never pinned, and keeps what the device wrote");
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

    /// A VMO of |nr_pages| anonymous pages, nothing committed.
    auto make_anonymous(usize const nr_pages) -> ustl::Arc<mm::VmObjectPaged> {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::ALLOC_USER_VMO, nr_pages * P, mm::VmoFlags::Lazy, &vmo) !=
            Status::Ok) {
            return {};
        }
        return vmo;
    }

    auto write_word(mm::VmObjectPaged &vmo, usize const index, u64 const value) -> bool {
        return vmo.write(&value, index * P, sizeof(value)) == Status::Ok;
    }

    auto check_zero_dedup(Tally &tally) -> void {
        auto vmo = make_anonymous(6);
        constexpr u64 SOMETHING = 0x5eed'0000'0000'0001;
        // Pages 0 and 1 committed and zero, 2 written, 3 written zero and then
        // something, 4 and 5 holes.
        if (!vmo || !write_word(*vmo, 0, 0) || !write_word(*vmo, 1, 0) ||
            !write_word(*vmo, 2, SOMETHING) || !write_word(*vmo, 3, 0) ||
            !write_word(*vmo, 3, SOMETHING)) {
            verdict(tally, false, "an anonymous VMO with zero and written pages is made");
            return;
        }
        auto const first = filed_at(*vmo, 0);
        if (!first.page || first.kind != Kind::AnonymousZeroFork ||
            vmo->committed_pages(0, 6 * P) != 4) {
            verdict(tally, false, "the committed pages are on the zero-fork queue");
            return;
        }
        auto &queues = queues_of(*first.page);

        auto const freed = queues.dedup_zero_pages(~usize{0});
        verdict(tally,
                freed >= 2 && !filed_at(*vmo, 0).page && !filed_at(*vmo, 1).page &&
                    vmo->committed_pages(0, 6 * P) == 2,
                "pages committed with zeros become zero markers and are freed");

        u64 a = 1;
        u64 b = 1;
        u64 c = 0;
        u64 d = 0;
        auto const read = word_at(*vmo, 0, &a) && word_at(*vmo, 1, &b) && word_at(*vmo, 2, &c) &&
                          word_at(*vmo, 3, &d);
        verdict(tally,
                read && a == 0 && b == 0 && c == SOMETHING && d == SOMETHING &&
                    filed_at(*vmo, 2).kind == Kind::Anonymous &&
                    filed_at(*vmo, 3).kind == Kind::Anonymous,
                "every byte reads as before; a page with something in it is left, and not looked "
                "at again");

        queues.dedup_zero_pages(~usize{0});
        verdict(tally,
                vmo->committed_pages(0, 6 * P) == 2 && filed_at(*vmo, 2).page &&
                    filed_at(*vmo, 3).page,
                "a second pass leaves them where they are");

        u64 e = 0;
        auto const rewritten = write_word(*vmo, 0, SOMETHING) && word_at(*vmo, 0, &e);
        verdict(tally,
                rewritten && e == SOMETHING && vmo->committed_pages(0, 6 * P) == 3 &&
                    filed_at(*vmo, 0).kind == Kind::AnonymousZeroFork,
                "a write to a marker commits a new page");
    }

    /// Puts the calling thread on |next| and loads its page tables, and hands
    /// back what it was on before; as usercopy.test does, with preemption off
    /// across the two steps.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    auto check_zero_mapped(Tally &tally) -> void {
        auto vmo = make_anonymous(2);
        ustl::Arc<mm::VmAspace> aspace;
        if (!vmo || !write_word(*vmo, 0, 0) ||
            mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                                 mm::VmasFlags::None, "pagereclaim.test", &aspace) != Status::Ok) {
            verdict(tally, false, "a VMO with a zero page and a user address space are made");
            return;
        }
        auto root = aspace->root_vma();
        auto mapping = root->map_with_vmo(
            VirtAddr::Anywhere, 2 * P, 0,
            mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User,
            mm::VmaFlags::PermMask, vmo, mm::VmMapOption::Populate, "pagereclaim.test");
        auto const page = filed_at(*vmo, 0).page;
        if (mapping.is_err() || !page) {
            verdict(tally, false, "the VMO is mapped");
        } else {
            auto const base = mapping.unwrap()->base();
            PhysAddr pa{};
            mm::MmuFlags flags{};
            auto const mapped = aspace->arch_aspace().query(base, &pa, &flags) == Status::Ok &&
                                pa == mm::frame_to_phys(page) && !!(flags & mm::MmuFlags::Writable);
            queues_of(*page).dedup_zero_pages(~usize{0});
            auto const gone = aspace->arch_aspace().query(base, &pa, &flags);
            verdict(tally, mapped && gone == Status::NotFound,
                    "a zero page is taken out of the mapping that shows it");
        }
        if (aspace->destroy() != Status::Ok) {
            verdict(tally, false, "the address space is destroyed");
        }
    }

    struct Race {
        mm::VmAspace *aspace = nullptr;
        VirtAddr base{};
        std::atomic<bool> done{false};
        std::atomic<usize> lost{0};
        std::atomic<usize> rounds{0};
    };

    constexpr usize RACE_PAGES = 8;
    constexpr usize RACE_ROUNDS = 20000;

    /// Writes something into a page of the mapping, reads it back, and writes
    /// zero again, so that the pages keep turning into zero pages for the
    /// other thread to take. A write that does not read back was lost.
    auto race_routine(void *const arg) -> i32 {
        auto &race = *static_cast<Race *>(arg);
        auto *const previous = attach(race.aspace);
        for (usize i = 1; i <= RACE_ROUNDS; ++i) {
            auto const at = race.base + (i % RACE_PAGES) * P;
            u64 const value = i;
            u64 const zero = 0;
            u64 back = 0;
            if (user_copy::copy_to_user(at, &value, sizeof(value)) != Status::Ok ||
                user_copy::copy_from_user(&back, at, sizeof(back)) != Status::Ok || back != value) {
                race.lost.fetch_add(1, std::memory_order_relaxed);
            }
            if (user_copy::copy_to_user(at, &zero, sizeof(zero)) != Status::Ok) {
                race.lost.fetch_add(1, std::memory_order_relaxed);
            }
            race.rounds.fetch_add(1, std::memory_order_relaxed);
        }
        attach(previous);
        race.done.store(true, std::memory_order_release);
        return 0;
    }

    auto check_zero_race(Tally &tally) -> void {
        auto vmo = make_anonymous(RACE_PAGES);
        ustl::Arc<mm::VmAspace> aspace;
        if (!vmo ||
            mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                                 mm::VmasFlags::None, "pagereclaim.test", &aspace) != Status::Ok) {
            verdict(tally, false, "a VMO and a user address space are made for the race");
            return;
        }
        auto root = aspace->root_vma();
        auto mapping = root->map_with_vmo(
            VirtAddr::Anywhere, RACE_PAGES * P, 0,
            mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User,
            mm::VmaFlags::PermMask, vmo, mm::VmMapOption::None, "pagereclaim.test");
        Race race;
        if (mapping.is_err()) {
            verdict(tally, false, "the VMO is mapped for the race");
        } else {
            race.aspace = aspace.as_ptr_mut();
            race.base = mapping.unwrap()->base();
            auto *const writer =
                task::Thread::spawn("zero-writer", race_routine, &race, task::DEFAULT_PRIORITY);
            if (!writer || writer->resume() != Status::Ok) {
                verdict(tally, false, "the thread that writes is started");
            } else {
                usize freed = 0;
                while (!race.done.load(std::memory_order_acquire)) {
                    mm::PmNode::for_each_node(mm::PmNode::State::Online,
                                              [&freed](mm::PmNode &node) {
                                                  freed += node.page_queues().dedup_zero_pages(8);
                                              });
                    task::Thread::Current::yield();
                }
                i32 code = 0;
                auto const joined = writer->join(&code, time::Deadline::infinite()) == Status::Ok;
                log::info("pagereclaim.test:   {} writes, {} lost, {} zero pages freed meanwhile",
                          race.rounds.load(), race.lost.load(), freed);
                verdict(tally,
                        joined && code == 0 && race.rounds.load() == RACE_ROUNDS &&
                            race.lost.load() == 0 && freed != 0,
                        "a program writing a page while it is deduplicated never loses the write");
            }
        }
        if (aspace->destroy() != Status::Ok) {
            verdict(tally, false, "the address space is destroyed after the race");
        }
    }

    /// Waits for |done| to hold, up to a few seconds.
    template <typename Done>
    auto settle(Done &&done) -> bool {
        auto const give_up = time::SteadyClock::now() + Seconds(5);
        while (!done()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            auto _ = task::Thread::Current::sleep_for(Milliseconds(10), task::Interruptible::No);
        }
        return true;
    }

    constexpr usize PRESSURE_PAGES = 64;
    constexpr usize DEBOUNCE = 8;

    auto levels_at(usize const out_of_memory, usize const critical, usize const warning)
        -> ustl::Result<mm::MemoryPressureLevels, Status> {
        return mm::MemoryPressureLevels::make(out_of_memory, critical, warning, DEBOUNCE,
                                              Duration::zero());
    }

    /// An anonymous VMO of zero pages and a page source's VMO of supplied
    /// ones, PRESSURE_PAGES each, for the reclaimer to find.
    struct Giveable {
        ustl::Arc<mm::VmObjectPaged> zeros;
        ustl::Arc<mm::VmObjectPaged> pager;
        CountingProvider *provider = nullptr;

        auto make() -> bool {
            zeros = make_anonymous(PRESSURE_PAGES);
            pager = make_vmo(PRESSURE_PAGES, &provider);
            if (!zeros || !pager || !supply(*pager, 0, PRESSURE_PAGES)) {
                return false;
            }
            for (usize i = 0; i < PRESSURE_PAGES; ++i) {
                if (!write_word(*zeros, i, 0)) {
                    return false;
                }
            }
            return true;
        }

        auto nr_zero() -> usize {
            return zeros->committed_pages(0, PRESSURE_PAGES * P);
        }

        auto nr_pager() -> usize {
            return pager->committed_pages(0, PRESSURE_PAGES * P);
        }
    };

    auto check_pressure(Tally &tally) -> void {
        auto &watchdog = mm::MemoryWatchdog::get();
        auto const boot = mm::MemoryPressureLevels::from_boot_options();

        // Nothing older than what is made here for the reclaimer to find first.
        mm::MemoryReclaimer::get().reclaim(~usize{0});

        // Critical, asked for about 25 frames: 16 to the end of Critical, and
        // 9 more when that is only the Warning below it.
        Giveable giveable;
        if (!giveable.make()) {
            verdict(tally, false, "zero pages and pager pages are made for the reclaimer");
            return;
        }
        auto const use_levels = [&](usize const out_of_memory, usize const critical,
                                    usize const warning) -> Status {
            auto const levels = levels_at(out_of_memory, critical, warning);
            if (!levels) {
                return Status::InvArgs;
            }
            return watchdog.set_levels(*levels);
        };
        auto free = mm::approx_free_frame_count();
        auto set = use_levels(free - 40, free + 8, free + 17);
        auto const started = set == Status::Ok && settle([&] { return giveable.nr_zero() < 64; });
        // Time for the reclaimer to finish what it was asked.
        auto _ = task::Thread::Current::sleep_for(Milliseconds(300), task::Interruptible::No);
        auto const zero_left = giveable.nr_zero();
        auto const pager_left = giveable.nr_pager();
        log::info("pagereclaim.test:   asked at Critical: {} of {} zero pages left, {} of {} pager "
                  "pages left",
                  zero_left, PRESSURE_PAGES, pager_left, PRESSURE_PAGES);
        verdict(tally,
                started && zero_left != 0 && zero_left <= PRESSURE_PAGES - 10 &&
                    pager_left == PRESSURE_PAGES,
                "at Critical the reclaimer gives back about what is asked, zero pages first");

        // Asked for more than there is: the zero pages go, and then the pager's.
        free = mm::approx_free_frame_count();
        set = use_levels(free - 40, free + 200, free + 209);
        auto const drained = set == Status::Ok &&
                             settle([&] { return giveable.nr_zero() + giveable.nr_pager() == 0; });
        verdict(tally, drained,
                "asked for more than the zero pages, it gives back pager pages too");

        // OutOfMemory, with pages to give: given back before anything is
        // refused, and the pressure that is found afterwards is declared.
        Giveable more;
        if (!more.make()) {
            verdict(tally, false, "pages are made for the reclaimer at OutOfMemory");
            auto _ = watchdog.set_levels(boot);
            return;
        }
        free = mm::approx_free_frame_count();
        set = use_levels(free + 40, free + 80, free + 100);
        auto const gave = set == Status::Ok && settle([&] { return more.nr_zero() < 64; });
        auto const served = mm::alloc_frames(mm::ALLOC_USER_VMO);
        auto const pressure = watchdog.pressure();
        verdict(tally, gave && pressure != mm::MemoryPressure::OutOfMemory && !!served,
                "at OutOfMemory with pages to give, they are given back, and an allocation that "
                "could wait is served");
        if (served) {
            mm::free_frames(*served, 0);
        }

        auto _ = watchdog.set_levels(boot);
        settle([&] { return watchdog.pressure() == mm::MemoryPressure::Normal; });
    }

    auto check_targeted_reclaim(Tally &tally) -> void {
        auto const local = mm::current_node();
        NodeId remote = MAX_NODES;
        for (NodeId nid = 0; nid < MAX_NODES; ++nid) {
            if (nid != local && mm::PmNode::node_memory_mask().test(nid) &&
                !mm::node_work_cpus(nid).any()) {
                remote = nid;
                break;
            }
        }
        if (remote == MAX_NODES) {
            verdict(tally, false, "targeted reclaim needs a memory-only node");
            return;
        }

        CountingProvider *local_provider = nullptr;
        CountingProvider *remote_provider = nullptr;
        auto local_vmo = make_vmo(1, &local_provider);
        auto remote_vmo = make_vmo(1, &remote_provider);
        if (!local_vmo || !remote_vmo || !supply(*local_vmo, 0, 1, local) ||
            !supply(*remote_vmo, 0, 1, remote)) {
            verdict(tally, false, "prepare one reclaimable pager page on each node");
            return;
        }

        mm::PmFrame::List held;
        mm::PmNode::node(remote)->zone_list()->for_each_local_zone([&held](mm::PmZone &zone) {
            for (usize order = MAX_FRAME_ORDER + 1; order != 0; --order) {
                while (auto block = zone.take_block(order - 1)) {
                    held.push_back(block.value());
                }
            }
        });

        auto const levels = mm::MemoryWatchdog::get().levels();
        auto const globally_normal =
            levels.pressure_at(mm::approx_free_frame_count()) == mm::MemoryPressure::Normal;
        auto const missed = mm::alloc_frames(remote, mm::ALLOC_KERNEL | mm::Gaf::ThisNode, 0);
        auto const exhausted = !missed && missed.unwrap_err() == Status::OutOfMem;
        if (missed) {
            mm::free_frames(*missed, 0);
        }
        auto const evicted = settle([&] { return remote_vmo->committed_pages(0, P) == 0; });
        auto const kept_local = local_vmo->committed_pages(0, P) == 1;
        bool retried = false;
        if (evicted) {
            auto const recovered =
                mm::alloc_frames(remote, mm::ALLOC_KERNEL | mm::Gaf::ThisNode, 0);
            if (recovered) {
                retried = (*recovered)->nid() == remote;
                mm::free_frames(*recovered, 0);
            }
        }
        mm::free_frames(&held);
        verdict(
            tally, globally_normal && exhausted && evicted && retried && kept_local,
            "a node-only shortage reclaims and resupplies a page without evicting the other node");
    }

    auto pagereclaim_test_command(int argc, char **argv) -> int {
        if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--target-node")) {
            log::error("usage: pagereclaim.test [--target-node]");
            return 1;
        }
        Tally tally;
        if (argc == 2) {
            check_targeted_reclaim(tally);
        }
        check_order_and_refetch(tally);
        check_mappings(tally);
        check_unpin_keeps_the_write(tally);
        check_lifetime(tally);
        check_zero_dedup(tally);
        check_zero_mapped(tally);
        check_zero_race(tally);
        check_pressure(tally);

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
