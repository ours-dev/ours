/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Pager.RequestsAsPortPacketsAndProxyLifetime: a pager VMO's requests as
// packets on a port, through its PagerProxy (.agents/tasks/user-pager, step
// 9).
//
// The test plays the pager: it reads the port and answers with
// supply_pages(). Faults come from kernel threads attached to an address
// space of the test's own, as in pagesource.test.
//
//   - A read fault on a page nobody supplied sends a Read packet with the
//     VMO's key and the page's range. One packet is out at a time: a second
//     fault's request waits in the proxy, and goes out when the pager reads
//     the first. A supply wakes both faults.
//   - A request withdrawn while its packet is still queued -- its fault gave
//     up after the proxy's overtime -- takes the packet back off the port.
//   - A detach sends Complete, with no range.
//   - The proxy and its PageSource hold each other until the port can no
//     longer have the packet, and then both go, whichever goes first: the
//     VMO with Complete read later, the VMO with Complete never read and the
//     port closed after, or the port closed before the VMO.
//   - The pager object: a VMO it makes reports its koid; it supplies a VMO's
//     pages out of another VMO and detaches it, and refuses another pager's
//     VMO. When it closes first, its VMOs are detached, what is on the port
//     is taken back, and the proxies go, at once or with their VMO.
//
// Why an in-kernel test: what is under test is a thread asleep in a page
// fault the MMU raised, waiting on a port a second thread reads.
#include <ours/arch/aspace_layout.hpp>
#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/pager_dispatcher.hpp>
#include <ours/object/pager_proxy.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>
#include <ours/vmo.hpp>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using object::KernelHandle;
    using object::PagerDispatcher;
    using object::PagerProxy;
    using object::PortDispatcher;
    using object::VmoDispatcher;
    using task::Thread;

    constexpr u64 P = PAGE_SIZE;
    constexpr u64 KEY = 0x9a9e'0000'0000'00aa;
    constexpr u64 OWNER = 0x9a9e'0000'0000'0001;
    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// Short enough that a fault nobody answers gives up within the test.
    constexpr auto QUICK_OVERTIME =
        PagerProxy::Overtime{.warn_every = Milliseconds(20), .give_up_after = Milliseconds(60)};

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
    }

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

    /// What every supplied page holds: its own index, so a page that lands at
    /// the wrong offset shows.
    auto pattern(u64 const offset) -> u64 {
        return 0x9a9e'50c0'0000'0000 | (offset / P);
    }

    /// Supplies [offset, offset + len) of |vmo|, each page filled with its
    /// pattern(), as a pager's pager_supply_pages would.
    auto supply(mm::VmObjectPaged &vmo, u64 const offset, u64 const len) -> Status {
        mm::VmPageSpliceList pages(len / P);
        if (auto const status = pages.reserve(len / P); Status::Ok != status) {
            return status;
        }
        for (u64 at = offset; at < offset + len; at += P) {
            auto *const frame = mm::alloc_frames(mm::ALLOC_KERNEL).value_or(nullptr);
            if (!frame) {
                return Status::OutOfMem;
            }
            auto *const words =
                reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(frame)));
            for (usize n = 0; n < P / sizeof(u64); ++n) {
                words[n] = pattern(at);
            }
            auto _ = pages.append_page((at - offset) / P, frame);
        }
        return vmo.supply_pages(offset, len, &pages, mm::VmObject::SupplyOption::Default);
    }

    /// A load from |va| by a thread of its own, which may sleep in the fault.
    struct Access {
        VirtAddr va{};
        u64 value = 0;
        Status status = Status::InternalError;
    };

    auto access_routine(void *const arg) -> i32 {
        auto &access = *static_cast<Access *>(arg);
        access.status = user_copy::copy_from_user(&access.value, access.va, sizeof(access.value));
        return 0;
    }

    auto start(mm::VmAspace *const aspace, Access *const access) -> Thread * {
        auto *const thread =
            Thread::spawn("pager-access", access_routine, access, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_aspace(aspace);
        auto _ = thread->resume();
        return thread;
    }

    /// Joins |thread|; one still asleep after LOST_AFTER is woken by
    /// detaching |vmo|, so |access| is never left in use.
    auto finish(Thread *const thread, mm::VmObject &vmo) -> bool {
        i32 retcode = 0;
        if (thread->join(&retcode, deadline_after(LOST_AFTER)) == Status::Ok) {
            return true;
        }
        log::error("pager: an access did not finish; detaching its VMO");
        vmo.detach_source();
        auto _ = thread->join(&retcode, time::Deadline::infinite());
        return false;
    }

    /// The next packet on |port|, waited for up to LOST_AFTER.
    auto next_packet(PortDispatcher &port, PortPacket *const out) -> bool {
        return poll_until([&] { return port.dequeue(0, out) == Status::Ok; });
    }

    auto is_read(PortPacket const &packet, u64 const offset, u64 const length) -> bool {
        return packet.type == PacketType::PageRequest && packet.key == KEY &&
               packet.status == Status::Ok &&
               packet.page_request.command == PageRequestCommand::Read &&
               packet.page_request.offset == offset && packet.page_request.length == length;
    }

    auto is_complete(PortPacket const &packet) -> bool {
        return packet.type == PacketType::PageRequest && packet.key == KEY &&
               packet.page_request.command == PageRequestCommand::Complete &&
               packet.page_request.offset == 0 && packet.page_request.length == 0;
    }

    /// A port, a proxy over it and a VMO of |pages| pages over the proxy's
    /// source. The test keeps weak references to the proxy and the source, to
    /// see them go.
    struct Pager {
        KernelHandle<PortDispatcher> handle;
        ustl::Arc<PortDispatcher> port;
        ustl::Arc<mm::VmObjectPaged> vmo;
        ustl::Weak<PagerProxy, ustl::sync::ThreadSafePolicy> proxy;
        ustl::Weak<mm::PageSource, ustl::sync::ThreadSafePolicy> source;
    };

    auto make_pager(usize const pages, PagerProxy::Overtime const overtime, Pager *const out)
        -> bool {
        Rights rights = Rights::None;
        if (PortDispatcher::create(PortCreateOptions::None, &out->handle, &rights) != Status::Ok) {
            return false;
        }
        out->port = out->handle.dispatcher();
        ustl::Arc<PagerProxy> proxy;
        ustl::Arc<mm::PageSource> source;
        if (PagerProxy::create(out->port, KEY, OWNER, overtime, nullptr, &proxy, &source) !=
            Status::Ok) {
            return false;
        }
        out->proxy = proxy.downgrade();
        out->source = source.downgrade();
        return mm::VmObjectPaged::with_source(source, mm::ALLOC_USER, pages * P, mm::VmoFlags::Lazy,
                                              &out->vmo) == Status::Ok;
    }

    /// Both halves of the cycle are gone: neither the proxy nor its source is
    /// referenced any more.
    auto both_gone(Pager const &pager) -> bool {
        return pager.proxy.expired() && pager.source.expired();
    }

    /// Requests, one packet at a time, a supply, a withdrawal and a detach,
    /// through a mapping of the VMO.
    auto check_requests(mm::VmAspace &aspace) -> void {
        Pager pager;
        if (!make_pager(4, PagerProxy::DEFAULT_OVERTIME, &pager)) {
            FAIL("allocate a port, a proxy and a VMO over it");
        }
        auto const owner = pager.vmo->source_owner_id();
        EXPECT_TRUE(owner.is_some() && *owner == OWNER,
                    "the VMO reports the owner id its proxy was made with");

        auto root = aspace.root_vma();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping =
            root->map_vmo(VirtAddr::Anywhere, 4 * P, 0, rw, mm::VmaFlags::PermMask,
                          ustl::Arc<mm::VmObject>(pager.vmo), mm::MapOptions::Lazy, "pager.test");
        if (mapping.is_err()) {
            FAIL("map the VMO");
        }
        auto const base = mapping.unwrap()->base();

        // How many requests the source has raised, to know every fault
        // started is waiting before the port is read.
        auto const raised = [&pager] {
            auto const source = pager.source.upgrade();
            if (!source) {
                return usize{0};
            }
            return source->nr_raised();
        };

        // Two faults before the pager reads anything. The proxy has one
        // packet: the first request goes out in it, the second waits in the
        // proxy and goes out once the pager has read the first.
        Access first{.va = base};
        auto *const first_thread = start(&aspace, &first);
        auto const first_asked = poll_until([&] { return raised() == 1; });
        Access second{.va = base + P};
        auto *const second_thread = start(&aspace, &second);
        auto const both_asked = poll_until([&] { return raised() == 2; });
        PortPacket packet{};
        EXPECT_TRUE(first_asked && both_asked && next_packet(*pager.port, &packet) &&
                        is_read(packet, 0, P),
                    "a read fault sends a Read packet with the VMO's key and the page's range");
        EXPECT_TRUE(next_packet(*pager.port, &packet) && is_read(packet, P, P),
                    "a second fault's request, made while the packet was out, goes out in it once "
                    "the first is read");

        EXPECT_OK(supply(*pager.vmo, 0, 2 * P), "pages 0 and 1 are supplied");
        auto const done = first_thread != nullptr && finish(first_thread, *pager.vmo) &&
                          second_thread != nullptr && finish(second_thread, *pager.vmo);
        EXPECT_TRUE(done && first.status == Status::Ok && first.value == pattern(0) &&
                        second.status == Status::Ok && second.value == pattern(P),
                    "both faults wake and read the supplied bytes");

        // Two more, unread: one packet on the port, one request in the proxy.
        // A detach takes both back and sends Complete as the only packet.
        Access third{.va = base + 2 * P};
        auto *const third_thread = start(&aspace, &third);
        auto const third_asked = poll_until([&] { return raised() == 1; });
        Access fourth{.va = base + 3 * P};
        auto *const fourth_thread = start(&aspace, &fourth);
        auto const fourth_asked = poll_until([&] { return raised() == 2; });
        pager.vmo->detach_source();
        auto const failed = third_thread != nullptr && finish(third_thread, *pager.vmo) &&
                            fourth_thread != nullptr && finish(fourth_thread, *pager.vmo) &&
                            third.status != Status::Ok && fourth.status != Status::Ok;
        auto const complete = next_packet(*pager.port, &packet) && is_complete(packet);
        EXPECT_TRUE(third_asked && fourth_asked && failed && complete &&
                        pager.port->dequeue(0, &packet) == Status::TimeOut,
                    "a detach fails the faults waiting, takes back the Read still on the port, and "
                    "sends Complete, with no range, as the only packet");

        auto _ = root->unmap(base, 4 * P);
    }

    /// A fault the pager never answers gives up after the proxy's overtime,
    /// and its request comes back off the port: nothing is left to read.
    auto check_withdrawal(mm::VmAspace &aspace) -> void {
        Pager pager;
        if (!make_pager(1, QUICK_OVERTIME, &pager)) {
            FAIL("allocate a proxy with a short overtime");
        }
        auto root = aspace.root_vma();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_vmo(VirtAddr::Anywhere, P, 0, rw, mm::VmaFlags::PermMask,
                                     ustl::Arc<mm::VmObject>(pager.vmo), mm::MapOptions::Lazy,
                                     "pager.test overtime");
        if (mapping.is_err()) {
            FAIL("map the VMO with the short overtime");
        }
        auto const base = mapping.unwrap()->base();

        Access unanswered{.va = base};
        auto *const thread = start(&aspace, &unanswered);
        auto const done = thread != nullptr && finish(thread, *pager.vmo);
        PortPacket packet{};
        EXPECT_TRUE(done && unanswered.status != Status::Ok &&
                        pager.port->dequeue(0, &packet) == Status::TimeOut,
                    "a fault nobody answers gives up after the overtime, and its packet is taken "
                    "back off the port");
        auto _ = root->unmap(base, P);
    }

    /// A request withdrawn while another waits on it hands its range to the
    /// waiter (PageSource::cancel_request_locked()), and the proxy carries on
    /// with the waiter wherever the withdrawn one was: its packet read, its
    /// packet still queued, or waiting in the proxy for the packet. The
    /// requests are raised on the source directly, with no fault, so that the
    /// test decides when each is withdrawn; each range is a page of its own,
    /// since a supply over a page already there completes nothing.
    auto check_handover() -> void {
        Pager pager;
        if (!make_pager(5, QUICK_OVERTIME, &pager)) {
            FAIL("allocate a proxy and a VMO of five pages");
        }
        auto source = pager.source.upgrade();
        auto &port = *pager.port;
        PortPacket packet{};

        // The pager has read the packet and is answering the range already:
        // asking again would have it supply the range twice.
        {
            mm::PageRequest primary;
            mm::PageRequest waiter;
            auto const raised = source->request_pages(0, P, &primary) == Status::ShouldWait &&
                                source->request_pages(0, P, &waiter) == Status::ShouldWait;
            auto const read = next_packet(port, &packet) && is_read(packet, 0, P);
            primary.cancel();
            auto const asked_again = port.dequeue(0, &packet) == Status::Ok;
            auto const woken =
                supply(*pager.vmo, 0, P) == Status::Ok && waiter.wait(false) == Status::Ok;
            EXPECT_TRUE(raised && read && !asked_again && woken,
                        "a request withdrawn after its packet was read is not asked for again, and "
                        "its waiter wakes on the one supply");
        }

        // The packet is still queued, ahead of one queued after it: it stays
        // there, for the waiter.
        {
            mm::PageRequest primary;
            mm::PageRequest waiter;
            auto const raised = source->request_pages(P, P, &primary) == Status::ShouldWait &&
                                source->request_pages(P, P, &waiter) == Status::ShouldWait;
            PortPacket behind{};
            behind.key = KEY + 1;
            auto const queued = port.queue_user(behind) == Status::Ok;
            primary.cancel();
            auto const first = next_packet(port, &packet) && is_read(packet, P, P);
            auto const second = next_packet(port, &packet) && packet.type == PacketType::User &&
                                packet.key == KEY + 1;
            auto const woken =
                supply(*pager.vmo, P, P) == Status::Ok && waiter.wait(false) == Status::Ok;
            EXPECT_TRUE(raised && queued && first && second && woken,
                        "a request withdrawn while its packet is queued leaves the packet where it "
                        "is, ahead of a packet queued after it, for its waiter");
        }

        // The packet carries another request; the withdrawn one waits in the
        // proxy ahead of a third. The waiter keeps its place in that line.
        {
            mm::PageRequest carried;
            mm::PageRequest primary;
            mm::PageRequest later;
            mm::PageRequest waiter;
            auto const raised = source->request_pages(2 * P, P, &carried) == Status::ShouldWait &&
                                source->request_pages(3 * P, P, &primary) == Status::ShouldWait &&
                                source->request_pages(4 * P, P, &later) == Status::ShouldWait &&
                                source->request_pages(3 * P, P, &waiter) == Status::ShouldWait;
            primary.cancel();
            auto const in_order = next_packet(port, &packet) && is_read(packet, 2 * P, P) &&
                                  next_packet(port, &packet) && is_read(packet, 3 * P, P) &&
                                  next_packet(port, &packet) && is_read(packet, 4 * P, P);
            auto const woken = supply(*pager.vmo, 2 * P, 3 * P) == Status::Ok &&
                               carried.wait(false) == Status::Ok &&
                               waiter.wait(false) == Status::Ok && later.wait(false) == Status::Ok;
            EXPECT_TRUE(raised && in_order && woken,
                        "a request withdrawn while it waits in the proxy leaves its waiter in its "
                        "place, ahead of a request made after it");
        }
        EXPECT_TRUE(port.dequeue(0, &packet) == Status::TimeOut,
                    "nothing is left on the port after the hand-overs");
    }

    /// Whichever goes first, the proxy and its source go in the end.
    auto check_lifetimes() -> void {
        PortPacket packet{};
        {
            // The VMO goes; its Complete is read afterwards.
            Pager pager;
            auto const made = make_pager(1, PagerProxy::DEFAULT_OVERTIME, &pager);
            pager.vmo.reset();
            auto const kept = !pager.source.expired();
            auto const read = next_packet(*pager.port, &packet) && is_complete(packet);
            EXPECT_TRUE(made && kept && read && both_gone(pager),
                        "a VMO gone with its Complete unread keeps the proxy and source until the "
                        "pager reads it, and then both go");
        }
        {
            // The VMO goes; the port is closed with Complete still on it.
            Pager pager;
            auto const made = make_pager(1, PagerProxy::DEFAULT_OVERTIME, &pager);
            pager.vmo.reset();
            auto const kept = !pager.source.expired();
            pager.handle.reset(ustl::Arc<PortDispatcher>());
            EXPECT_TRUE(made && kept && both_gone(pager),
                        "closing the port drops the Complete still on it, and both go");
        }
        {
            // The port is closed first; the VMO goes after.
            Pager pager;
            auto const made = make_pager(1, PagerProxy::DEFAULT_OVERTIME, &pager);
            pager.handle.reset(ustl::Arc<PortDispatcher>());
            pager.vmo.reset();
            EXPECT_TRUE(made && both_gone(pager),
                        "with the port closed first, a Complete it cannot queue is as good as "
                        "delivered, and both go with the VMO");
        }
    }

    /// A pager object, a port, and a VMO the pager made, handed out as
    /// handles as the system calls will hand them.
    struct Made {
        KernelHandle<PagerDispatcher> pager;
        KernelHandle<PortDispatcher> port;
        KernelHandle<VmoDispatcher> vmo;
    };

    auto make_with_pager(u64 const size, Made *const out) -> bool {
        Rights rights = Rights::None;
        return PagerDispatcher::create(0, out->pager, rights) == Status::Ok &&
               PortDispatcher::create(PortCreateOptions::None, &out->port, &rights) == Status::Ok &&
               out->pager.dispatcher()->create_vmo(VmoOptions::None, out->port.dispatcher(), KEY,
                                                   size, out->vmo, rights) == Status::Ok;
    }

    /// The pager object's operations: a VMO made, its koid checked, pages
    /// supplied from another VMO, a detach, and another pager refused.
    auto check_dispatcher() -> void {
        Made made;
        if (!make_with_pager(3 * P, &made)) {
            FAIL("allocate a pager, a port and a VMO the pager made");
        }
        auto pager = made.pager.dispatcher();
        auto port = made.port.dispatcher();
        auto vmo = made.vmo.dispatcher();
        Rights rights = Rights::None;
        KernelHandle<VmoDispatcher> plain;
        auto const plain_made =
            VmoDispatcher::create_user(P, VmoOptions::None, plain, rights) == Status::Ok;
        EXPECT_TRUE(vmo->pager_koid() == pager->koid() && plain_made &&
                        plain.dispatcher()->pager_koid() == KOID_INVALID,
                    "a VMO the pager made reports the pager's koid; any other VMO, none");

        // The pager's content, prepared in a VMO of its own and moved in.
        KernelHandle<VmoDispatcher> aux;
        u64 const word = 0x5a5a'0000'0000'0001;
        u64 got = 0;
        u64 left = ~u64{0};
        auto const supplied =
            VmoDispatcher::create_user(P, VmoOptions::None, aux, rights) == Status::Ok &&
            aux.dispatcher()->write(&word, 0, sizeof(word)) == Status::Ok &&
            pager->supply_pages(*vmo, P, P, *aux.dispatcher(), 0) == Status::Ok &&
            vmo->read(&got, P, sizeof(got)) == Status::Ok &&
            aux.dispatcher()->read(&left, 0, sizeof(left)) == Status::Ok;
        EXPECT_TRUE(
            supplied && got == word && left == 0,
            "supply_pages moves a page of another VMO into the pager's VMO, and leaves zero "
            "behind");
        EXPECT_TRUE(pager->supply_pages(*vmo, 1, P, *aux.dispatcher(), 0) == Status::InvArgs &&
                        pager->supply_pages(*vmo, 0, 0, *aux.dispatcher(), 0) == Status::Ok,
                    "an unaligned range is InvArgs, and an empty one is Ok and does nothing");

        Made other;
        auto const other_made = make_with_pager(P, &other);
        auto other_pager = other.pager.dispatcher();
        EXPECT_TRUE(
            other_made &&
                other_pager->supply_pages(*vmo, 0, P, *aux.dispatcher(), 0) == Status::InvArgs &&
                other_pager->detach_vmo(*vmo) == Status::InvArgs &&
                pager->detach_vmo(*plain.dispatcher()) == Status::InvArgs,
            "another pager can neither supply nor detach this pager's VMO, nor a pager a VMO "
            "no pager made");

        PortPacket packet{};
        EXPECT_TRUE(pager->detach_vmo(*vmo) == Status::Ok && next_packet(*port, &packet) &&
                        is_complete(packet),
                    "detach_vmo sends Complete to the VMO's port");
    }

    /// The pager closes first. With its VMO still alive: the waiting fault
    /// fails, nothing is left on the port, and the proxy lives on with the
    /// VMO only, going with it. With the VMO gone and Complete unread: the
    /// Complete is taken back and the proxy goes at once. And no VMO is made
    /// once the pager has closed.
    auto check_pager_closes_first(mm::VmAspace &aspace) -> void {
        PortPacket packet{};
        {
            auto const before = PagerProxy::nr_alive();
            Made made;
            if (!make_with_pager(2 * P, &made)) {
                FAIL("allocate a pager whose VMO outlives it");
            }
            auto port = made.port.dispatcher();
            auto vmo = made.vmo.dispatcher()->vmo();
            auto root = aspace.root_vma();
            auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
            auto mapping = root->map_vmo(VirtAddr::Anywhere, 2 * P, 0, rw, mm::VmaFlags::PermMask,
                                         vmo, mm::MapOptions::Lazy, "pager.test pager first");
            if (mapping.is_err()) {
                FAIL("map the VMO that outlives its pager");
            }
            auto const base = mapping.unwrap()->base();

            Access waiting{.va = base + P};
            auto *const thread = start(&aspace, &waiting);
            auto const asked = next_packet(*port, &packet) && is_read(packet, P, P);
            made.pager.reset(ustl::Arc<PagerDispatcher>());
            auto const failed =
                thread != nullptr && finish(thread, *vmo) && waiting.status != Status::Ok;
            EXPECT_TRUE(
                asked && failed && port->dequeue(0, &packet) == Status::TimeOut &&
                    PagerProxy::nr_alive() == before + 1,
                "a pager closed under its VMO fails the waiting fault, leaves nothing on the "
                "port, and its proxy lives on with the VMO");
            auto _ = root->unmap(base, 2 * P);
            vmo.reset();
            made.vmo.reset(ustl::Arc<VmoDispatcher>());
            EXPECT_EQ(PagerProxy::nr_alive(), before, "and goes with the VMO");
        }
        {
            auto const before = PagerProxy::nr_alive();
            Made made;
            auto const ok = make_with_pager(P, &made);
            auto pager = made.pager.dispatcher();
            made.vmo.reset(ustl::Arc<VmoDispatcher>());
            auto const kept = PagerProxy::nr_alive() == before + 1;
            made.pager.reset(ustl::Arc<PagerDispatcher>());
            auto port = made.port.dispatcher();
            KernelHandle<VmoDispatcher> late;
            Rights rights = Rights::None;
            EXPECT_TRUE(ok && kept && PagerProxy::nr_alive() == before &&
                            port->dequeue(0, &packet) == Status::TimeOut,
                        "a pager closed with its VMO gone and Complete unread takes the Complete "
                        "back, and the proxy goes");
            EXPECT_TRUE(pager->create_vmo(VmoOptions::None, port, KEY, P, late, rights) ==
                            Status::BadState,
                        "no VMO is made once the pager has closed");
        }
    }

    KTEST(Pager, RequestsAsPortPacketsAndProxyLifetime) {
        ustl::Arc<mm::VmAspace> aspace;
        ASSERT_OK(mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                       mm::VmasKind::User, mm::VmasFlags::None, "pager.test",
                                       &aspace),
                  "create the address space");
        check_requests(*aspace);
        check_withdrawal(*aspace);
        check_handover();
        check_lifetimes();
        check_dispatcher();
        check_pager_closes_first(*aspace);
        if (auto const status = aspace->destroy(); status != Status::Ok) {
            log::error("pager: could not destroy the address space: {}", to_string(status));
        }
    }
} // namespace
} // namespace ours
