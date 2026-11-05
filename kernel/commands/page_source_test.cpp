/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `pagesource.test`: pages that come from a provider -- asked for by a page
// fault, merged when several threads ask for the same ones, supplied or failed
// later, and waited on in between.
//
// A test provider stands in for whoever has the content. It only queues what
// it is sent; the command itself plays the pager: it takes requests off that
// queue and answers them with VmObjectPaged::supply_pages() or
// PageSource::on_pages_failed(). That keeps every step in one place and in a
// known order, so the checks can say exactly what the provider should have
// seen at each point.
//
// Seven groups of checks:
//
//   - PageSource on its own, with requests on this thread's stack: what a
//     request asks for after it is cut against the ones outstanding, which
//     ones the provider sees, a partial supply, a failure, a withdrawal that
//     hands the request to a waiter, and detach.
//   - A zero marker in a node with a source, through the node's cursor: zero
//     content the provider is never asked for, and that a supply leaves alone.
//   - Through the page fault path, on a VMO with a source mapped into an
//     address space of the test's own: a read fault sleeps until its page is
//     supplied and reads the supplied bytes; three threads faulting on one
//     page cause one provider request and all read the same bytes; a failed
//     page fails the fault; a write fault asks for the content first.
//   - Through the VMO's own copies: read(), write() and read_user() of a page
//     nobody supplied sleep until it is supplied, as a fault does, instead of
//     handing ShouldWait to their caller.
//   - A populate mapping of a VMO half supplied: it maps what is there, asks
//     for nothing, and leaves the rest to a fault that asks.
//   - A hand-over: take_pages() out of an anonymous VMO and supply_pages() of
//     what it took, a page, a zero marker, a gap and a page, into one with a
//     source, waking a read that waits on the gap.
//   - detach_source(): a waiting read fails, what was supplied stays, a later
//     fault fails without asking, a supply is refused; and the provider's
//     owner id, as a VMO reports it.
//
// Why a console command: what is under test is a thread put to sleep inside a
// page fault the MMU raised, which no host test can produce. The faulting
// threads are kernel threads attached to the test's address space, as
// usercopy.test's command thread is.
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using mm::PageRequest;
    using mm::PageSource;
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("pagesource.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pagesource.test: FAIL  {}", name);
    }

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
    }

    /// Polls |condition| until it holds or LOST_AFTER has passed.
    template <typename Condition>
    auto poll_until(Condition const &condition) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!condition()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            auto _ = Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        return true;
    }

    /// The owner id every TestProvider reports, as a pager reports its koid.
    constexpr u64 TEST_OWNER = 0x7e57'0000'0000'0001;

    /// A provider that queues what it is sent and does nothing else: the test
    /// takes requests off the queue and answers them itself.
    class TestProvider final: public mm::PageProvider {
      public:
        [[nodiscard]]
        auto owner_id() const -> ustl::Option<u64> override {
            return ustl::some(TEST_OWNER);
        }

        /// A request as the provider saw it, copied out under the lock.
        struct Taken {
            u64 offset;
            u64 len;
        };

        /// The oldest request queued, taken off the queue.
        auto take() -> ustl::Option<Taken> {
            Guard guard{m_lock};
            auto front = m_queue.front();
            if (!front) {
                return ustl::NONE;
            }
            auto const taken = Taken{.offset = offset(*front), .len = len(*front)};
            m_queue.pop_front();
            return ustl::some(taken);
        }

        auto nr_sent() -> usize {
            Guard guard{m_lock};
            return m_nr_sent;
        }

        auto nr_queued() -> usize {
            Guard guard{m_lock};
            return m_queue.size();
        }

        auto closed() -> bool {
            Guard guard{m_lock};
            return m_detached && m_closed;
        }

        auto detached() -> bool {
            Guard guard{m_lock};
            return m_detached;
        }

        /// How long a faulting thread waits before it gives up with TimeOut.
        /// Changed only while no access thread is running: a thread reads it
        /// after start() and the test changes it after finish().
        auto set_wait_limit(Duration const limit) -> void {
            m_wait_limit = limit;
        }

      private:
        auto send_async_request(PageRequest *const request) -> void override {
            Guard guard{m_lock};
            m_queue.push_back(*request);
            m_nr_sent += 1;
        }

        /// The request is queued unless the test already took it.
        auto clear_async_request(PageRequest *const request) -> void override {
            Guard guard{m_lock};
            if (is_queued(*request)) {
                m_queue.erase(*request);
            }
        }

        /// The heir takes the withdrawn request's place in the queue; if the
        /// test already took that one, it is answering the range, and nothing
        /// changes.
        auto swap_async_request(PageRequest *const old, PageRequest *const heir) -> void override {
            Guard guard{m_lock};
            if (is_queued(*old)) {
                auto _ = m_queue.insert(m_queue.iterator_to(*old), *heir);
                m_queue.erase(*old);
            }
        }

        /// Bounded, so that a supply the test never makes fails a check rather
        /// than hanging the console.
        auto wait_on_event(sync::Event *const event, bool const suspendable) -> Status override {
            return event->wait(deadline_after(m_wait_limit),
                               suspendable ? task::Interruptible::Yes : task::Interruptible::No);
        }

        auto on_detach() -> void override {
            Guard guard{m_lock};
            m_detached = true;
        }

        auto on_close() -> void override {
            Guard guard{m_lock};
            m_closed = true;
        }

        sync::Mutex m_lock;
        PageRequest::ProviderList m_queue TA_GUARDED(m_lock);
        usize m_nr_sent TA_GUARDED(m_lock) = 0;
        bool m_detached TA_GUARDED(m_lock) = false;
        bool m_closed TA_GUARDED(m_lock) = false;
        Duration m_wait_limit = LOST_AFTER;
    };

    /// A provider and the source over it. |provider| stays valid while
    /// |source| holds it.
    struct Rig {
        TestProvider *provider = nullptr;
        ustl::Arc<PageSource> source;
    };

    auto make_rig(Rig *const rig) -> Status {
        auto *const provider = new (mm::ALLOC_KERNEL) TestProvider();
        if (!provider) {
            return Status::OutOfMem;
        }
        auto held = ustl::Arc<mm::PageProvider>::adopt(provider);
        if (auto const status = PageSource::create(ustl::move(held), &rig->source);
            Status::Ok != status) {
            return status;
        }
        rig->provider = provider;
        return Status::Ok;
    }

    auto is(ustl::Option<TestProvider::Taken> const &taken, u64 const offset, u64 const len)
        -> bool {
        return taken.is_some() && taken->offset == offset && taken->len == len;
    }

    /// PageSource alone. Each request here is signalled before it is waited
    /// on, so every wait() returns at once.
    auto check_source(Tally &tally) -> void {
        constexpr u64 P = PAGE_SIZE;

        Rig rig;
        if (auto const status = make_rig(&rig); Status::Ok != status) {
            log::error("pagesource.test: could not make a page source: {}", to_string(status));
            tally.failed += 1;
            return;
        }
        auto &source = *rig.source;
        auto &provider = *rig.provider;

        // a = [0, 4P), sent. b asks for [2P, 6P); it starts inside a, so it
        // waits on a and is cut to a's end, [2P, 4P). c = [6P, 8P), sent.
        // d asks for [4P, 8P) and runs into c, so it is cut to [4P, 6P), sent.
        PageRequest a;
        PageRequest b;
        PageRequest c;
        PageRequest d;
        auto const raised = source.get_pages(0, 4 * P, &a) == Status::ShouldWait &&
                            source.get_pages(2 * P, 4 * P, &b) == Status::ShouldWait &&
                            source.get_pages(6 * P, 2 * P, &c) == Status::ShouldWait &&
                            source.get_pages(4 * P, 4 * P, &d) == Status::ShouldWait;
        verdict(tally, raised, "get_pages() answers ShouldWait");
        verdict(tally, provider.nr_sent() == 3 && source.nr_raised() == 4,
                "a request that starts inside an outstanding one is not sent");

        auto const first = provider.take();
        auto const second = provider.take();
        auto const third = provider.take();
        verdict(tally, is(first, 0, 4 * P) && is(second, 6 * P, 2 * P) && is(third, 4 * P, 2 * P),
                "a request that runs into an outstanding one stops where it starts");

        source.on_pages_supplied(0, 2 * P);
        verdict(tally, source.nr_raised() == 4, "a partial supply completes nothing");

        source.on_pages_supplied(2 * P, 2 * P);
        verdict(tally, source.nr_raised() == 2, "the rest completes the request and its waiter");
        verdict(tally, a.wait(false) == Status::Ok && b.wait(false) == Status::Ok,
                "the request and its waiter both see Ok");

        source.on_pages_failed(4 * P, 4 * P, Status::BadState);
        verdict(tally, source.nr_raised() == 0, "a failure completes the requests it covers");
        verdict(tally, c.wait(false) == Status::BadState && d.wait(false) == Status::BadState,
                "a failed request reports the failure");

        // e is sent, f waits on it. Withdrawing e must not strand f: f takes
        // its place in the provider's queue, and nothing new is sent.
        PageRequest e;
        PageRequest f;
        auto _ = source.get_pages(0, 2 * P, &e);
        auto _ = source.get_pages(0, 2 * P, &f);
        auto const sent_before = provider.nr_sent();
        e.cancel();
        verdict(tally,
                provider.nr_sent() == sent_before && provider.nr_queued() == 1 &&
                    source.nr_raised() == 1,
                "withdrawing a request someone waits on hands its place to the waiter");
        verdict(tally, is(provider.take(), 0, 2 * P), "the waiter asks for the same range");
        source.on_pages_supplied(0, 2 * P);
        verdict(tally, f.wait(false) == Status::Ok && source.nr_raised() == 0,
                "the waiter is completed by the supply");

        // A waiter that leaves changes nothing for the request it waited on.
        PageRequest g;
        PageRequest h;
        auto _ = source.get_pages(0, P, &g);
        auto _ = source.get_pages(0, P, &h);
        h.cancel();
        verdict(tally, source.nr_raised() == 1 && provider.nr_queued() == 1,
                "a waiter that leaves only leaves");
        g.cancel();
        verdict(tally, source.nr_raised() == 0 && provider.nr_queued() == 0,
                "the last one leaves the provider too");

        PageRequest i;
        auto _ = source.get_pages(0, P, &i);
        source.close();
        verdict(tally, i.wait(false) == Status::Ok && source.nr_raised() == 0,
                "closing completes what is outstanding");
        PageRequest j;
        verdict(tally, source.get_pages(0, P, &j) == Status::BadState,
                "a closed source raises nothing");
        verdict(tally, provider.closed(), "the provider hears of detach and close");
    }

    auto is_zero(mm::VmPage *const page) -> bool {
        auto const *const bytes = mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(page));
        for (usize i = 0; i < PAGE_SIZE; ++i) {
            if (bytes[i] != 0) {
                return false;
            }
        }
        return true;
    }

    /// A zero marker in a node with a page source: content the source already
    /// gave, as zeros. Through the node's own cursor, on this thread, so
    /// nothing here waits: a read of the marker is the shared zero page and
    /// asks the provider nothing; a request for the pages before it stops at
    /// it; a page supplied over it is thrown away; and a write puts a zeroed
    /// page in its place, again without asking.
    ///
    /// The analysis cannot tie `node->lock()` to the lock the cursor and the
    /// supply require; it is held from the guard to the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto check_zero_markers(Tally &tally) -> void {
        constexpr u64 P = PAGE_SIZE;

        Rig rig;
        ustl::Arc<mm::VmCowPages> node;
        if (make_rig(&rig) != Status::Ok ||
            mm::VmCowPages::with_source(rig.source, mm::ALLOC_USER, 3 * P, &node) != Status::Ok) {
            verdict(tally, false, "a node with a source is made for the marker checks");
            return;
        }
        auto &provider = *rig.provider;
        Guard guard{node->lock()};

        verdict(tally, node->add_zero_marker_locked(1) == Status::Ok,
                "a zero marker is put at page 1");
        {
            auto cursor = node->make_cursor(P, P);
            mm::MultiPageRequest request;
            auto const read = cursor.require_page(false, 1, &request);
            verdict(tally,
                    read && read->page == mm::get_zero_page() && !read->writable &&
                        provider.nr_sent() == 0 && node->nr_pages_locked() == 0,
                    "a read of it is the zero page, read-only, and asks the provider nothing");
        }
        {
            // Withdrawn when |request| goes, at the end of the block.
            auto cursor = node->make_cursor(0, 3 * P);
            mm::MultiPageRequest request;
            auto const read = cursor.require_page(false, 3, &request);
            verdict(tally,
                    read.is_err() && read.unwrap_err() == Status::ShouldWait &&
                        is(provider.take(), 0, P),
                    "a request for the pages before it stops at it");
        }
        {
            mm::VmPageSpliceList pages(1);
            auto *const frame =
                mm::alloc_frames(mm::ALLOC_KERNEL | mm::Gaf::Zero).value_or(nullptr);
            auto held = frame != nullptr && pages.reserve(1) == Status::Ok &&
                        pages.append_page(0, frame) == Status::Ok;
            if (!held && frame != nullptr) {
                mm::free_frames(frame, 0);
            }
            auto const supplied = held && node->supply_pages_locked(P, P, &pages) == Status::Ok;
            auto cursor = node->make_cursor(P, P);
            mm::MultiPageRequest request;
            auto const read = cursor.require_page(false, 1, &request);
            verdict(tally,
                    supplied && read && read->page == mm::get_zero_page() &&
                        node->nr_pages_locked() == 0,
                    "a page supplied over it is thrown away, and it still reads as zero");
        }
        {
            auto cursor = node->make_cursor(P, P);
            mm::MultiPageRequest request;
            auto const written = cursor.require_page(true, 1, &request);
            verdict(tally,
                    written && written->writable && written->page != mm::get_zero_page() &&
                        is_zero(written->page) && node->nr_pages_locked() == 1 &&
                        provider.nr_sent() == 1,
                    "a write of it gets a zeroed page in its place, and asks the provider nothing");
        }
    }

    /// What every page of the test VMO is supplied with: its own index, so a
    /// page that lands at the wrong offset shows.
    auto pattern(u64 const offset) -> u64 {
        return 0x9a9e'50c0'0000'0000 | (offset / PAGE_SIZE);
    }

    /// Supplies [offset, offset + len) of |vmo|, each page filled with its
    /// pattern(). What supply_pages() did not take the list frees.
    auto supply(mm::VmObjectPaged &vmo, u64 const offset, u64 const len) -> Status {
        mm::VmPageSpliceList pages(len / PAGE_SIZE);
        if (auto const status = pages.reserve(len / PAGE_SIZE); Status::Ok != status) {
            return status;
        }
        for (u64 at = offset; at < offset + len; at += PAGE_SIZE) {
            auto *const frame = mm::alloc_frames(mm::ALLOC_KERNEL).value_or(nullptr);
            if (!frame) {
                return Status::OutOfMem;
            }
            // Through u8: phys_to_virt<u64> also names the overload that
            // returns an address, since VirtAddr is a u64.
            auto *const words =
                reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(frame)));
            for (usize n = 0; n < PAGE_SIZE / sizeof(u64); ++n) {
                words[n] = pattern(at);
            }
            // Room was made for every page, so this cannot fail.
            auto _ = pages.append_page((at - offset) / PAGE_SIZE, frame);
        }
        return vmo.supply_pages(offset, len, &pages, mm::VmObject::SupplyOption::Default);
    }

    /// One access to a page that may not be there yet, by a thread of its own,
    /// which may sleep until the page is supplied.
    struct Access {
        enum class Kind {
            /// A load from |va|, a mapping of the VMO: a read fault.
            Load,
            /// A store of |value| to |va|: a write fault.
            Store,
            /// vmo->read() of |offset| into |value|.
            VmoRead,
            /// vmo->write() of |value| to |offset|.
            VmoWrite,
            /// vmo->read_user() of |offset| into the user buffer at |va|, then a
            /// load of it into |value|.
            VmoReadUser,
            /// vmo->commit_range() of [|offset|, |offset| + |len|).
            VmoCommit,

            /// vmo->pin_range() of [|offset|, |offset| + |len|).
            VmoPin,
        };

        Kind kind = Kind::Load;
        VirtAddr va{};
        mm::VmObjectPaged *vmo = nullptr;
        u64 offset = 0;
        /// Length of the commit or pin request.
        u64 len = 0;
        /// Written, or read back.
        u64 value = 0;
        Status status = Status::InternalError;
    };

    auto access_routine(void *const arg) -> i32 {
        auto &access = *static_cast<Access *>(arg);
        auto const size = sizeof(access.value);
        switch (access.kind) {
            case Access::Kind::Load:
                access.status = user_copy::copy_from_user(&access.value, access.va, size);
                break;
            case Access::Kind::Store:
                access.status = user_copy::copy_to_user(access.va, &access.value, size);
                break;
            case Access::Kind::VmoRead:
                access.status = access.vmo->read(&access.value, access.offset, size);
                break;
            case Access::Kind::VmoWrite:
                access.status = access.vmo->write(&access.value, access.offset, size);
                break;
            case Access::Kind::VmoReadUser:
                access.status = access.vmo->read_user(user_copy::UserOutPtr<u8>(access.va),
                                                      access.offset, size);
                if (Status::Ok == access.status) {
                    access.status = user_copy::copy_from_user(&access.value, access.va, size);
                }
                break;
            case Access::Kind::VmoCommit:
                access.status = access.vmo->commit_range(access.offset, access.len,
                                                         mm::VmObject::CommitOptions::None);
                break;
            case Access::Kind::VmoPin:
                access.status = access.vmo->pin_range(access.offset, access.len);
                break;
        }
        return 0;
    }

    auto start(mm::VmAspace *const aspace, Access *const access) -> Thread * {
        auto *const thread =
            Thread::spawn("pagesource-access", access_routine, access, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_aspace(aspace);
        auto _ = thread->resume();
        return thread;
    }

    /// Joins |thread|. One that is still asleep after LOST_AFTER is waiting on
    /// a request nobody will answer: closing the source wakes it, its retry
    /// fails, and it exits, so |access| is never left in use when the command
    /// returns.
    auto finish(Thread *const thread, PageSource &source) -> bool {
        i32 retcode = 0;
        if (thread->join(&retcode, deadline_after(LOST_AFTER)) == Status::Ok) {
            return true;
        }
        log::error("pagesource.test: an access did not finish; closing the source");
        source.close();
        auto _ = thread->join(&retcode, time::Deadline::infinite());
        return false;
    }

    /// The VMO's own copies of pages 4 to 6, none of them supplied yet: each
    /// sleeps until its page is, and then copies the supplied bytes. Then a
    /// commit of pages 7 and 8, which waits for each in turn.
    auto check_copies(Tally &tally, mm::VmAspace &aspace, Rig &rig, mm::VmObjectPaged &vmo)
        -> void {
        constexpr u64 P = PAGE_SIZE;
        constexpr u64 WRITTEN = 0x7e57'c0b1'e500'0005;

        auto &source = *rig.source;
        auto &provider = *rig.provider;

        // read(), from the middle of page 4.
        {
            Access reader{.kind = Access::Kind::VmoRead, .vmo = &vmo, .offset = 4 * P + 16};
            auto *const thread = start(&aspace, &reader);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && is(provider.take(), 4 * P, P),
                    "read() of a page nobody supplied asks for it");
            verdict(tally, supply(vmo, 4 * P, P) == Status::Ok, "page 4 is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            log::info("pagesource.test:   read(): {} read {:#x}", to_string(reader.status),
                      reader.value);
            verdict(tally, done && reader.status == Status::Ok && reader.value == pattern(4 * P),
                    "read() waits for the page and returns its bytes");
        }

        // write(), at the start of page 5.
        {
            Access writer{
                .kind = Access::Kind::VmoWrite, .vmo = &vmo, .offset = 5 * P, .value = WRITTEN};
            auto *const thread = start(&aspace, &writer);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && is(provider.take(), 5 * P, P),
                    "write() to a page nobody supplied asks for it");
            verdict(tally, supply(vmo, 5 * P, P) == Status::Ok, "page 5 is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            u64 words[2] = {};
            auto const read = vmo.read(words, 5 * P, sizeof(words));
            log::info("pagesource.test:   write(): {}, page 5 holds {:#x} {:#x}",
                      to_string(writer.status), words[0], words[1]);
            verdict(tally,
                    done && writer.status == Status::Ok && read == Status::Ok &&
                        words[0] == WRITTEN && words[1] == pattern(5 * P),
                    "write() waits for the page and lands on it");
        }

        // read_user() of page 6 into a user buffer that is not there yet
        // either: the walk waits for the page, then faults on the buffer, and
        // carries on from where it was after each.
        auto buffer = aspace.root_vma()->map_anon(VirtAddr::Anywhere, P,
                                                  mm::MmuFlags::Readable | mm::MmuFlags::Writable |
                                                      mm::MmuFlags::User,
                                                  mm::MapOptions::Lazy, "pagesource.test buffer");
        if (buffer.is_err()) {
            log::error("pagesource.test: could not map the buffer: {}",
                       to_string(buffer.unwrap_err()));
            tally.failed += 1;
            return;
        }
        auto const buffer_base = buffer.unwrap()->base();
        {
            Access reader{
                .kind = Access::Kind::VmoReadUser, .va = buffer_base, .vmo = &vmo, .offset = 6 * P};
            auto *const thread = start(&aspace, &reader);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && is(provider.take(), 6 * P, P),
                    "read_user() of a page nobody supplied asks for it");
            verdict(tally, supply(vmo, 6 * P, P) == Status::Ok, "page 6 is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            log::info("pagesource.test:   read_user(): {} read {:#x}", to_string(reader.status),
                      reader.value);
            verdict(tally, done && reader.status == Status::Ok && reader.value == pattern(6 * P),
                    "read_user() waits for the page, then fills the user buffer");
        }
        auto _ = aspace.root_vma()->unmap(buffer_base, P);

        // commit_range() of pages 7 and 8: it waits for page 7, and goes on
        // from page 8, not from the start, once page 7 is there.
        {
            Access committer{
                .kind = Access::Kind::VmoCommit, .vmo = &vmo, .offset = 7 * P, .len = 2 * P};
            auto *const thread = start(&aspace, &committer);
            auto const first = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, first && is(provider.take(), 7 * P, P),
                    "commit_range() of pages nobody supplied asks for the first");
            verdict(tally, supply(vmo, 7 * P, P) == Status::Ok, "page 7 is supplied");
            auto const second = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, second && is(provider.take(), 8 * P, P),
                    "the commit goes on from page 8 and asks for it");
            verdict(tally, supply(vmo, 8 * P, P) == Status::Ok, "page 8 is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            log::info("pagesource.test:   commit_range(): {}, {} page(s) committed",
                      to_string(committer.status), vmo.committed_pages(7 * P, 2 * P));
            verdict(tally,
                    done && committer.status == Status::Ok &&
                        vmo.committed_pages(7 * P, 2 * P) == 2,
                    "commit_range() waits for each page and commits both");
        }
    }

    auto check_faults(Tally &tally, mm::VmAspace &aspace, Rig &rig) -> void {
        constexpr u64 P = PAGE_SIZE;
        constexpr u64 WRITTEN = 0x0123'4567'89ab'cdef;

        auto &source = *rig.source;
        auto &provider = *rig.provider;

        // Pages 0 to 3 are mapped and faulted on; 4 to 6 are reached through
        // the VMO's own copies only.
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (auto const status = mm::VmObjectPaged::with_source(rig.source, mm::ALLOC_USER, 9 * P,
                                                               mm::VmoFlags::Lazy, &vmo);
            Status::Ok != status) {
            log::error("pagesource.test: could not make the VMO: {}", to_string(status));
            tally.failed += 1;
            return;
        }

        auto root = aspace.root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_with_vmo(VirtAddr::Anywhere, 4 * P, 0, flags,
                                          mm::VmaFlags::PermMask, ustl::Arc<mm::VmObject>(vmo),
                                          mm::MapOptions::Lazy, "pagesource.test");
        if (mapping.is_err()) {
            log::error("pagesource.test: could not map the VMO: {}",
                       to_string(mapping.unwrap_err()));
            tally.failed += 1;
            return;
        }
        auto const base = mapping.unwrap()->base();

        // One reader, page 0.
        {
            Access reader{.va = base};
            auto *const thread = start(&aspace, &reader);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && provider.nr_sent() == 1,
                    "a read fault on a page nobody supplied asks the provider");
            verdict(tally, is(provider.take(), 0, P), "it asks for the page that faulted");
            verdict(tally, supply(*vmo, 0, P) == Status::Ok, "the page is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            log::info("pagesource.test:   reader: {} read {:#x}", to_string(reader.status),
                      reader.value);
            verdict(tally, done && reader.status == Status::Ok && reader.value == pattern(0),
                    "the fault resumes and reads the supplied bytes");
        }

        // Three readers, page 1: one request between them.
        {
            Access readers[3] = {{.va = base + P}, {.va = base + P}, {.va = base + P}};
            Thread *threads[3] = {};
            for (usize n = 0; n < 3; ++n) {
                threads[n] = start(&aspace, &readers[n]);
            }
            auto const asked = poll_until([&] { return source.nr_raised() == 3; });
            verdict(tally, asked && provider.nr_sent() == 2 && provider.nr_queued() == 1,
                    "three faults on one page send one request");
            verdict(tally, is(provider.take(), P, P), "it asks for that page");
            verdict(tally, supply(*vmo, P, P) == Status::Ok, "the page is supplied once");
            auto all_read = true;
            for (usize n = 0; n < 3; ++n) {
                auto const done = threads[n] != nullptr && finish(threads[n], source);
                all_read = all_read && done && readers[n].status == Status::Ok &&
                           readers[n].value == pattern(P);
            }
            verdict(tally, all_read, "all three resume and read the same bytes");
        }

        // One reader, page 2, and the page fails.
        {
            Access reader{.va = base + 2 * P};
            auto *const thread = start(&aspace, &reader);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && is(provider.take(), 2 * P, P), "page 2 is asked for");
            source.on_pages_failed(2 * P, P, Status::BadState);
            auto const done = thread != nullptr && finish(thread, source);
            log::info("pagesource.test:   failed reader: {}", to_string(reader.status));
            verdict(tally, done && reader.status != Status::Ok, "a failed page fails the fault");
        }

        // One writer, page 3: it needs the content before it can write.
        {
            Access writer{.kind = Access::Kind::Store, .va = base + 3 * P + 8, .value = WRITTEN};
            auto *const thread = start(&aspace, &writer);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && is(provider.take(), 3 * P, P),
                    "a write fault asks for the content first");
            verdict(tally, supply(*vmo, 3 * P, P) == Status::Ok, "page 3 is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            u64 words[2] = {};
            auto const read = vmo->read(words, 3 * P, sizeof(words));
            log::info("pagesource.test:   writer: {}, page 3 holds {:#x} {:#x}",
                      to_string(writer.status), words[0], words[1]);
            verdict(tally,
                    done && writer.status == Status::Ok && read == Status::Ok &&
                        words[0] == pattern(3 * P) && words[1] == WRITTEN,
                    "the write lands on the supplied page");
        }

        // Page 2 again, failed above and still missing, and this time nobody
        // answers: the wait times out, the fault path shows the address space
        // under its lock, and the fault fails with the request withdrawn.
        {
            provider.set_wait_limit(Milliseconds(50));
            auto const sent_before = provider.nr_sent();
            Access reader{.va = base + 2 * P};
            auto *const thread = start(&aspace, &reader);
            auto const done = thread != nullptr && finish(thread, source);
            provider.set_wait_limit(LOST_AFTER);
            log::info("pagesource.test:   unanswered reader: {}", to_string(reader.status));
            verdict(tally,
                    done && reader.status != Status::Ok && provider.nr_sent() == sent_before + 1,
                    "a fault nobody answers times out and fails");
            verdict(tally, source.nr_raised() == 0 && provider.nr_queued() == 0,
                    "the timed-out request is withdrawn from the source and the provider");
        }

        verdict(tally, supply(*vmo, 0, P) == Status::Ok && source.nr_raised() == 0,
                "a page supplied twice keeps the first and wakes nobody");

        check_copies(tally, aspace, rig, *vmo);
        verdict(tally, provider.nr_queued() == 0, "nothing is left with the provider");

        auto _ = root->unmap(base, 4 * P);
    }

    /// A populate mapping of a VMO with a source, half supplied: it maps the
    /// supplied page and asks the provider for nothing, and the page nobody
    /// supplied is left to a fault, which asks for it.
    auto check_populate(Tally &tally, mm::VmAspace &aspace, Rig &rig) -> void {
        constexpr u64 P = PAGE_SIZE;
        auto &source = *rig.source;
        auto &provider = *rig.provider;

        ustl::Arc<mm::VmObjectPaged> vmo;
        if (auto const status = mm::VmObjectPaged::with_source(rig.source, mm::ALLOC_USER, 3 * P,
                                                               mm::VmoFlags::Lazy, &vmo);
            Status::Ok != status) {
            log::error("pagesource.test: could not make the VMO to populate: {}",
                       to_string(status));
            tally.failed += 1;
            return;
        }
        verdict(tally, supply(*vmo, P, P) == Status::Ok,
                "page 1 of an object with a source is supplied, page 0 is not");

        auto root = aspace.root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const where = root->base() + 16_mb;
        auto const commit = root->map_with_vmo(where, 2 * P, 0, flags, mm::VmaFlags::PermMask, vmo,
                                               mm::MapOptions::Commit, "pager commit");
        ustl::Arc<mm::VmMapping> leftover;
        verdict(tally,
                commit.is_err() && commit.unwrap_err() == Status::ShouldWait &&
                    aspace.find_mapping(where, &leftover) == Status::NotFound &&
                    source.nr_raised() == 0 && provider.nr_queued() == 0,
                "a failed immediate commit releases the address and withdraws its request");

        auto const sent_before = provider.nr_sent();
        auto mapping = root->map_with_vmo(where, 2 * P, 0, flags, mm::VmaFlags::PermMask,
                                          ustl::Arc<mm::VmObject>(vmo), mm::MapOptions::Resident,
                                          "pagesource.test populate");
        verdict(tally, mapping.is_ok(), "a populate mapping of it is made");
        if (mapping.is_err()) {
            return;
        }
        auto const base = mapping.unwrap()->base();
        PhysAddr pa{};
        mm::MmuFlags mmuf{};
        auto const supplied_mapped = aspace.arch_aspace().query(base + P, &pa, &mmuf) == Status::Ok;
        auto const missing_mapped = aspace.arch_aspace().query(base, &pa, &mmuf) == Status::Ok;
        verdict(tally,
                supplied_mapped && !missing_mapped && provider.nr_sent() == sent_before &&
                    source.nr_raised() == 0,
                "it maps the supplied page, leaves the other to fault, and asks the provider for "
                "nothing");

        {
            Access reader{.va = base};
            auto *const thread = start(&aspace, &reader);
            auto const asked = poll_until([&] { return source.nr_raised() == 1; });
            verdict(tally, asked && is(provider.take(), 0, P),
                    "a read of the page left out asks the provider for it");
            verdict(tally, supply(*vmo, 0, P) == Status::Ok, "it is supplied");
            auto const done = thread != nullptr && finish(thread, source);
            verdict(tally, done && reader.status == Status::Ok && reader.value == pattern(0),
                    "the read resumes and reads the supplied bytes");
        }

        // The pin reaches page 1 before waiting for the missing page 2.
        provider.set_wait_limit(Milliseconds(50));
        auto const pin_status = vmo->pin_range(P, 2 * P);
        provider.set_wait_limit(LOST_AFTER);
        verdict(tally,
                pin_status == Status::TimeOut && vmo->unpin_range(P, P) == Status::BadState &&
                    vmo->committed_pages(P, P) == 1 && source.nr_raised() == 0,
                "a pin that times out rolls back the prefix without discarding its pages");
        Access pin{
            .kind = Access::Kind::VmoPin, .vmo = vmo.as_ptr_mut(), .offset = P, .len = 2 * P};
        auto *const thread = start(&aspace, &pin);
        auto const waiting = poll_until([&] { return source.nr_raised() == 1; });
        verdict(tally, waiting && is(provider.take(), 2 * P, P),
                "a retry waits for only the unsupplied page");
        verdict(tally, supply(*vmo, 2 * P, P) == Status::Ok, "the missing pin page is supplied");
        auto const done = thread != nullptr && finish(thread, source);
        verdict(tally,
                done && pin.status == Status::Ok && vmo->unpin_range(P, 2 * P) == Status::Ok &&
                    vmo->unpin_range(P, 2 * P) == Status::BadState,
                "the resumed pin holds each page exactly once");

        auto _ = root->unmap(base, 2 * P);
    }

    /// Puts a zero marker at page |index| of |vmo|'s own pages.
    ///
    /// The analysis cannot tie the node's lock to the one add_zero_marker_locked()
    /// requires; it is held from the guard to the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto mark_zero(mm::VmObjectPaged &vmo, usize const index) -> bool {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        return node->add_zero_marker_locked(index) == Status::Ok;
    }

    /// take_pages() out of an anonymous object and supply_pages() of what it
    /// took into one with a source, the way a pager hands over content it
    /// prepared: a page, a zero marker, a gap and a page, the last over a page
    /// supplied before, while a read waits on the gap.
    auto check_take_and_supply(Tally &tally, mm::VmAspace &aspace, Rig &rig) -> void {
        constexpr u64 P = PAGE_SIZE;
        constexpr u64 AUX_WORD = 0x7a00'0000'0000'0000;
        auto &source = *rig.source;
        auto &provider = *rig.provider;
        auto root = aspace.root_vma();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const present_at = [&aspace](VirtAddr const va, PhysAddr *const pa) {
            mm::MmuFlags mmuf{};
            return aspace.arch_aspace().query(va, pa, &mmuf) == Status::Ok;
        };

        // The pager's own object: page 0 and page 3 written, a zero marker at
        // page 1, nothing at page 2. Mapped, so that the take has entries to
        // unmap.
        ustl::Arc<mm::VmObjectPaged> aux;
        u64 const words[2] = {AUX_WORD | 0, AUX_WORD | 3};
        auto const aux_made = mm::VmObjectPaged::create(mm::ALLOC_USER, 4 * P, mm::VmoFlags::Lazy,
                                                        &aux) == Status::Ok &&
                              aux->write(&words[0], 0, sizeof(u64)) == Status::Ok &&
                              aux->write(&words[1], 3 * P, sizeof(u64)) == Status::Ok &&
                              mark_zero(*aux, 1);
        auto aux_map = root->map_with_vmo(VirtAddr::Anywhere, 4 * P, 0, rw, mm::VmaFlags::PermMask,
                                          ustl::Arc<mm::VmObject>(aux), mm::MapOptions::Resident,
                                          "pagesource.test aux");
        PhysAddr frame0{};
        auto const aux_ready =
            aux_made && aux_map.is_ok() && present_at(aux_map.unwrap()->base(), &frame0);
        verdict(tally, aux_ready,
                "an anonymous object holds a page, a zero marker, nothing, and a page, mapped");

        // The object with a source: page 3 supplied already, page 2 waited on.
        ustl::Arc<mm::VmObjectPaged> dst;
        auto const dst_made =
            mm::VmObjectPaged::with_source(rig.source, mm::ALLOC_USER, 4 * P, mm::VmoFlags::Lazy,
                                           &dst) == Status::Ok &&
            supply(*dst, 3 * P, P) == Status::Ok;
        auto dst_map = root->map_with_vmo(VirtAddr::Anywhere, 4 * P, 0, rw, mm::VmaFlags::PermMask,
                                          ustl::Arc<mm::VmObject>(dst), mm::MapOptions::Lazy,
                                          "pagesource.test dst");
        if (!aux_ready || !dst_made || dst_map.is_err()) {
            verdict(tally, false, "an object with a source, page 3 supplied, is mapped");
            return;
        }
        auto const dst_base = dst_map.unwrap()->base();
        Access reader{.va = dst_base + 2 * P};
        auto *const thread = start(&aspace, &reader);
        auto const asked = poll_until([&] { return source.nr_raised() == 1; });
        verdict(tally, asked && is(provider.take(), 2 * P, P), "a read of its page 2 waits");

        // The hand-over.
        mm::VmPageSpliceList pages(4);
        auto const taken = aux->take_pages(0, 4 * P, &pages) == Status::Ok;
        PhysAddr unused{};
        auto const aux_base = aux_map.unwrap()->base();
        verdict(tally,
                taken && aux->committed_pages(0, 4 * P) == 0 && !present_at(aux_base, &unused) &&
                    !present_at(aux_base + 3 * P, &unused),
                "take_pages() leaves the object nothing in the range, and its mapping no entry");
        auto const sent_before = provider.nr_sent();
        verdict(tally,
                dst->supply_pages(0, 4 * P, &pages, mm::VmObject::SupplyOption::Default) ==
                    Status::Ok,
                "what it took is supplied to the object with the source");

        auto const done = thread != nullptr && finish(thread, source);
        verdict(tally, done && reader.status == Status::Ok && reader.value == 0,
                "the gap arrives as a zero marker: the read waiting on it wakes and reads zero");

        u64 got[4] = {};
        auto const read_all = dst->read(&got[0], 0, sizeof(u64)) == Status::Ok &&
                              dst->read(&got[1], P, sizeof(u64)) == Status::Ok &&
                              dst->read(&got[2], 2 * P, sizeof(u64)) == Status::Ok &&
                              dst->read(&got[3], 3 * P, sizeof(u64)) == Status::Ok;
        log::info("pagesource.test:   after the hand-over: {:#x} {:#x} {:#x} {:#x}", got[0], got[1],
                  got[2], got[3]);
        verdict(tally,
                read_all && got[0] == words[0] && got[1] == 0 && got[2] == 0 &&
                    got[3] == pattern(3 * P) && provider.nr_sent() == sent_before &&
                    dst->committed_pages(0, 4 * P) == 2,
                "page 0 has the bytes it left with, pages 1 and 2 are zero without a page, page "
                "3 keeps what was supplied first, and the provider is asked nothing");
        PhysAddr frame0_now{};
        verdict(tally,
                aspace.soft_fault(dst_base, mm::VmfCause::Absent) == Status::Ok &&
                    present_at(dst_base, &frame0_now) && frame0_now == frame0,
                "page 0 is the very frame the anonymous object had: moved, not copied");

        // What take_pages() refuses.
        mm::VmPageSpliceList from_source(1);
        verdict(tally, dst->take_pages(0, P, &from_source) == Status::NotSupported,
                "nothing is taken from an object with a source");
        mm::VmPageSpliceList wrong_length(2);
        mm::VmPageSpliceList past_end(2);
        verdict(tally,
                aux->take_pages(0, P, &wrong_length) == Status::InvArgs &&
                    aux->take_pages(3 * P, 2 * P, &past_end) == Status::OutOfRange,
                "a list of the wrong length is InvArgs, a range past the end OutOfRange");
        ustl::Arc<mm::VmObjectPaged> pinned;
        mm::VmPageSpliceList from_pinned(1);
        verdict(tally,
                mm::VmObjectPaged::create(mm::ALLOC_USER, P, mm::VmoFlags::Pinned, &pinned) ==
                        Status::Ok &&
                    pinned->take_pages(0, P, &from_pinned) == Status::BadState,
                "and a pinned object is BadState");

        auto _ = root->unmap(aux_base, 4 * P);
        auto _ = root->unmap(dst_base, 4 * P);
    }

    /// detach_source() of a VMO with a source: the read waiting on a page
    /// nobody supplied fails, a page supplied before stays and reads, a later
    /// fault on a missing page fails without asking the provider, and a supply
    /// is refused. And the owner id the provider chose reaches the VMO and its
    /// slice, and a VMO with no source has none.
    auto check_detach(Tally &tally, mm::VmAspace &aspace, Rig &rig) -> void {
        constexpr u64 P = PAGE_SIZE;
        auto &source = *rig.source;
        auto &provider = *rig.provider;

        ustl::Arc<mm::VmObjectPaged> vmo;
        if (auto const status = mm::VmObjectPaged::with_source(rig.source, mm::ALLOC_USER, 3 * P,
                                                               mm::VmoFlags::Lazy, &vmo);
            Status::Ok != status) {
            log::error("pagesource.test: could not make the VMO to detach: {}", to_string(status));
            tally.failed += 1;
            return;
        }
        ustl::Arc<mm::VmObjectPaged> slice;
        ustl::Arc<mm::VmObjectPaged> plain;
        auto const owners =
            vmo->create_slice(P, P, &slice) == Status::Ok &&
            mm::VmObjectPaged::create(mm::ALLOC_USER, P, mm::VmoFlags::Lazy, &plain) == Status::Ok;
        auto const vmo_owner = vmo->source_owner_id();
        ustl::Option<u64> slice_owner;
        if (owners) {
            slice_owner = slice->source_owner_id();
        }
        verdict(tally,
                owners && vmo_owner.is_some() && *vmo_owner == TEST_OWNER &&
                    slice_owner.is_some() && *slice_owner == TEST_OWNER &&
                    plain->source_owner_id().is_none(),
                "the provider's owner id reaches the VMO and its slice; a VMO with no source has "
                "none");

        verdict(tally, supply(*vmo, 0, P) == Status::Ok, "page 0 is supplied, pages 1 and 2 not");
        auto root = aspace.root_vma();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_with_vmo(VirtAddr::Anywhere, 3 * P, 0, rw, mm::VmaFlags::PermMask,
                                          ustl::Arc<mm::VmObject>(vmo), mm::MapOptions::Lazy,
                                          "pagesource.test detach");
        if (mapping.is_err()) {
            verdict(tally, false, "the VMO to detach is mapped");
            return;
        }
        auto const base = mapping.unwrap()->base();

        Access waiting{.va = base + P};
        auto *const thread = start(&aspace, &waiting);
        auto const asked = poll_until([&] { return source.nr_raised() == 1; });
        verdict(tally, asked && is(provider.take(), P, P), "a read of page 1 waits");

        vmo->detach_source();
        auto const done = thread != nullptr && finish(thread, source);
        log::info("pagesource.test:   the waiting read after detach: {}",
                  to_string(waiting.status));
        verdict(tally, done && waiting.status != Status::Ok && provider.detached(),
                "detaching fails the read waiting on page 1, and tells the provider");

        u64 word = 0;
        verdict(tally, vmo->read(&word, 0, sizeof(word)) == Status::Ok && word == pattern(0),
                "page 0, supplied before, still reads (D5: what was supplied stays)");

        auto const sent_before = provider.nr_sent();
        Access late{.va = base + 2 * P};
        auto *const late_thread = start(&aspace, &late);
        auto const late_done = late_thread != nullptr && finish(late_thread, source);
        verdict(tally,
                late_done && late.status != Status::Ok && provider.nr_sent() == sent_before &&
                    source.nr_raised() == 0,
                "a fault on page 2 fails at once, and asks the provider nothing");
        verdict(tally, supply(*vmo, 2 * P, P) == Status::BadState, "and a supply is refused");

        auto _ = root->unmap(base, 3 * P);
    }

    auto pagesource_test_command(int, char **) -> int {
        Tally tally;
        check_source(tally);
        check_zero_markers(tally);

        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "pagesource.test", &aspace);
            status != Status::Ok) {
            log::error("pagesource.test: could not create an address space: {}", to_string(status));
            return 1;
        }

        Rig rig;
        if (auto const status = make_rig(&rig); Status::Ok != status) {
            log::error("pagesource.test: could not make a page source: {}", to_string(status));
            tally.failed += 1;
        } else {
            check_faults(tally, *aspace, rig);
        }

        // A source of its own: the one above was closed when its VMO went.
        Rig populate_rig;
        if (auto const status = make_rig(&populate_rig); Status::Ok != status) {
            log::error("pagesource.test: could not make a page source: {}", to_string(status));
            tally.failed += 1;
        } else {
            check_populate(tally, *aspace, populate_rig);
        }

        // And one for the hand-over, for the same reason.
        Rig take_rig;
        if (auto const status = make_rig(&take_rig); Status::Ok != status) {
            log::error("pagesource.test: could not make a page source: {}", to_string(status));
            tally.failed += 1;
        } else {
            check_take_and_supply(tally, *aspace, take_rig);
        }

        // And one to detach.
        Rig detach_rig;
        if (auto const status = make_rig(&detach_rig); Status::Ok != status) {
            log::error("pagesource.test: could not make a page source: {}", to_string(status));
            tally.failed += 1;
        } else {
            check_detach(tally, *aspace, detach_rig);
        }

        if (auto const status = aspace->destroy(); status != Status::Ok) {
            log::error("pagesource.test: could not destroy the address space: {}",
                       to_string(status));
            tally.failed += 1;
        }
        aspace.reset();

        // The VMO is gone: unmapped, and the test's own reference dropped when
        // check_faults() returned. Its pages closed the source as they went.
        if (rig.provider != nullptr) {
            verdict(tally, rig.provider->closed(), "the VMO closes its source when it goes");
        }

        if (tally.failed != 0) {
            log::error("pagesource.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pagesource.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PageSourceTest, "pagesource.test", ours::pagesource_test_command,
             "pagesource.test: page faults that wait for a provider to supply the page");
