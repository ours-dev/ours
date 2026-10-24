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
// Four groups of checks:
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
//
// Why a console command: what is under test is a thread put to sleep inside a
// page fault the MMU raised, which no host test can produce. The faulting
// threads are kernel threads attached to the test's address space, as
// usercopy.test's command thread is.
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
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

    /// A provider that queues what it is sent and does nothing else: the test
    /// takes requests off the queue and answers them itself.
    class TestProvider final: public mm::PageProvider {
      public:
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
        auto *const provider = new (mm::GAF_KERNEL) TestProvider();
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
        // its place and is sent in its stead.
        PageRequest e;
        PageRequest f;
        auto _ = source.get_pages(0, 2 * P, &e);
        auto _ = source.get_pages(0, 2 * P, &f);
        auto const sent_before = provider.nr_sent();
        e.cancel();
        verdict(tally,
                provider.nr_sent() == sent_before + 1 && provider.nr_queued() == 1 &&
                    source.nr_raised() == 1,
                "withdrawing a request someone waits on sends the waiter instead");
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
            mm::VmCowPages::with_source(rig.source, mm::GAF_USER, 3 * P, &node) != Status::Ok) {
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
            auto const read = cursor->require_page(false, 1, &request);
            verdict(tally,
                    read && read->page == mm::get_zero_page() && !read->writable &&
                        provider.nr_sent() == 0 && node->nr_pages_locked() == 0,
                    "a read of it is the zero page, read-only, and asks the provider nothing");
        }
        {
            // Withdrawn when |request| goes, at the end of the block.
            auto cursor = node->make_cursor(0, 3 * P);
            mm::MultiPageRequest request;
            auto const read = cursor->require_page(false, 3, &request);
            verdict(tally,
                    read.is_err() && read.unwrap_err() == Status::ShouldWait &&
                        is(provider.take(), 0, P),
                    "a request for the pages before it stops at it");
        }
        {
            mm::VmPage::List pages;
            if (auto *const frame = mm::alloc_frames(mm::GAF_KERNEL | mm::Gaf::Zero)) {
                pages.push_back(*frame);
            }
            auto const supplied = pages.front() &&
                                  node->supply_pages_locked(P, P, &pages) == Status::Ok &&
                                  !pages.front();
            auto cursor = node->make_cursor(P, P);
            mm::MultiPageRequest request;
            auto const read = cursor->require_page(false, 1, &request);
            verdict(tally,
                    supplied && read && read->page == mm::get_zero_page() &&
                        node->nr_pages_locked() == 0,
                    "a page supplied over it is thrown away, and it still reads as zero");
        }
        {
            auto cursor = node->make_cursor(P, P);
            mm::MultiPageRequest request;
            auto const written = cursor->require_page(true, 1, &request);
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
    /// pattern(). What supply_pages() did not take is freed here.
    auto supply(mm::VmObjectPaged &vmo, u64 const offset, u64 const len) -> Status {
        mm::VmPage::List pages;
        auto status = Status::Ok;
        for (u64 at = offset; at < offset + len; at += PAGE_SIZE) {
            auto *const frame = mm::alloc_frames(mm::GAF_KERNEL);
            if (!frame) {
                status = Status::OutOfMem;
                break;
            }
            // Through u8: phys_to_virt<u64> also names the overload that
            // returns an address, since VirtAddr is a u64.
            auto *const words =
                reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(frame)));
            for (usize n = 0; n < PAGE_SIZE / sizeof(u64); ++n) {
                words[n] = pattern(at);
            }
            pages.push_back(*frame);
        }

        if (Status::Ok == status) {
            status = vmo.supply_pages(offset, len, &pages, mm::VmObject::SupplyOption::Default);
        }
        while (auto left = pages.front()) {
            pages.pop_front();
            mm::free_frames(&*left, 0);
        }
        return status;
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
        };

        Kind kind = Kind::Load;
        VirtAddr va = 0;
        mm::VmObjectPaged *vmo = nullptr;
        u64 offset = 0;
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
    /// sleeps until its page is, and then copies the supplied bytes.
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
        auto buffer = aspace.root_vma()->map_anon(mm::VmPlacement::anywhere(), P,
                                                  mm::MmuFlags::Readable | mm::MmuFlags::Writable |
                                                      mm::MmuFlags::User,
                                                  mm::VmMapOption::None, "pagesource.test buffer");
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
    }

    auto check_faults(Tally &tally, mm::VmAspace &aspace, Rig &rig) -> void {
        constexpr u64 P = PAGE_SIZE;
        constexpr u64 WRITTEN = 0x0123'4567'89ab'cdef;

        auto &source = *rig.source;
        auto &provider = *rig.provider;

        // Pages 0 to 3 are mapped and faulted on; 4 to 6 are reached through
        // the VMO's own copies only.
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (auto const status = mm::VmObjectPaged::with_source(rig.source, mm::GAF_USER, 7 * P,
                                                               mm::VmoFlags::Lazy, &vmo);
            Status::Ok != status) {
            log::error("pagesource.test: could not make the VMO: {}", to_string(status));
            tally.failed += 1;
            return;
        }

        auto root = aspace.root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_with_vmo(mm::VmPlacement::anywhere(), 4 * P, 0, flags,
                                          mm::VmaFlags::PermMask, ustl::Arc<mm::VmObject>(vmo),
                                          mm::VmMapOption::None, "pagesource.test");
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
