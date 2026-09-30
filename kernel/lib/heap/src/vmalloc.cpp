/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <heap/new.hpp>
#include <heap/vmalloc.hpp>

#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/sync/mutex.hpp>

namespace heap {
using ours::Guard;
using ours::Status;
using ours::mm::MmuFlags;

namespace {
    /// Read/write and nothing else. A vmalloc range holds data -- buffers,
    /// tables, arenas -- so leaving it executable would hand every one of them
    /// to anyone looking for a place to put code, and no caller has wanted
    /// otherwise. A vmalloc_exec(), if this kernel ever loads modules, would be
    /// a separate entry point with its own justification.
    constexpr auto MMU_FLAGS = MmuFlags::Readable | MmuFlags::Writable;

    /// The window, as a sub-area of the kernel root VMA.
    ///
    /// This is what makes vmalloc a *client* of the VMM rather than a second
    /// implementation of it: the address inside the window is chosen by
    /// VmArea::alloc_spot(), the pages come from a VmObjectPaged, and the
    /// result is a VmMapping like any other -- so the range appears in the
    /// aspace dump, and retiring it goes through the same code that retires
    /// every other mapping.
    ///
    /// An earlier draft reserved the range and then drove alloc_frames() and
    /// ArchVmAspace::map() directly, with an address allocator of its own. It
    /// worked, and it was wrong: it grew a parallel copy of what VmArea and
    /// VmObject already are.
    ustl::Arc<ours::mm::VmArea> WINDOW;

    /// One record per live range, so vfree() can recover from a bare pointer
    /// both the length and the mapping to retire.
    ///
    /// Holding the Arc is not what keeps the mapping alive -- the parent's
    /// VmaSet has a reference of its own, and VmArea::unmap() is what retires
    /// one. It is here so vfree() need not search the VMA tree, and so the
    /// mapping cannot be recycled underneath a pointer we still answer for.
    ///
    /// A fixed table, because an allocator that allocates in order to record
    /// what it allocated is a recursion waiting to be discovered. The bound is
    /// generous: this kernel's live vmalloc ranges are counted on two hands.
    constexpr auto MAX_LIVE_RANGES = usize{256};

    ustl::Arc<ours::mm::VmMapping> BUSY[MAX_LIVE_RANGES];
    usize NR_BUSY;
    bool READY;

    /// Guards WINDOW and the table.
    ///
    /// A Mutex rather than a SpinLock, because everything under it can block:
    /// VmObjectPaged::create() commits through a sleeping mutex of its own and
    /// mm::alloc_frames() takes the current thread's resource lock. That is
    /// what makes "not callable from an interrupt handler" a property of the
    /// design rather than a warning in a comment.
    ///
    /// It does not currently deliver mutual exclusion, and this is the honest
    /// place to say so rather than a comment claiming otherwise.
    /// Mutex::acquire() calls acquire_common() and drops its bool
    /// (ours/sync/mutex.hpp:91); that bool means "did not acquire", and
    /// acquire_slowpath() returns it after breaking out of a spin that never
    /// parks -- its "Make current thread sleep" comment has no code behind it
    /// (kernel/kernel/sync/mutex.cpp:103). A contending caller therefore walks
    /// straight into the critical section. Filed with the full sequence in
    /// docs/repairs/260908/sync.md; it is a defect in the primitive, not here,
    /// and fixing it needs a wait queue rather than a change at this call site.
    ///
    /// What holds today is that every caller is single-threaded: the boot self
    /// check, prof's init hook, and userboot_init() all run before anything
    /// else allocates this large. That is a property of the current callers,
    /// not of this code, and it stops holding the first time a running thread
    /// asks `new` for more than KMALLOC_MAX_SIZE.
    ours::sync::Mutex LOCK;
} // namespace

auto init_vmalloc() -> void {
    Guard guard{LOCK};

    // At a fixed base, because is_vmalloc_addr() has to be a compile-time
    // range test -- that is what `operator delete` routes on. Reserved as a
    // sub-area rather than left to anywhere() for the same reason: the window
    // must be exactly where the constants say it is.
    auto const status = ours::mm::VmAspace::kernel_aspace()->root_vma()->create_subvma(
        ours::mm::VmPlacement::at(VMALLOC_BASE), VMALLOC_SIZE,
        ours::mm::VmaFlags::Read | ours::mm::VmaFlags::Write, "k:vmalloc",
        ours::mm::VmMapOption::None, &WINDOW);
    ASSERT(Status::Ok == status, "Failed to reserve the vmalloc window");

    NR_BUSY = 0;
    READY = true;
    log::info("vmalloc: window [{:X}, {:X}), {} pages", VMALLOC_BASE, VMALLOC_BASE + VMALLOC_SIZE,
              VMALLOC_SIZE / PAGE_SIZE);
}

auto vmalloc(usize const size, Gaf const gaf) -> void * {
    if (size == 0) {
        return nullptr;
    }

    auto const rounded = (size + PAGE_SIZE - 1) & ~usize(PAGE_SIZE - 1);
    // The round-up wraps for a size within a page of the top of the address
    // space, and a wrapped length would be mapped as nothing and freed as
    // though it were huge.
    if (rounded < size || rounded > VMALLOC_SIZE) {
        return nullptr;
    }

    Guard guard{LOCK};

    // Reachable, and reported rather than asserted: `operator new` routes here
    // by size, and the kernel allocates before init_vmm() runs (SparseMemMap
    // builds its arrays at InitLevel::Pmm). A caller that asks too early gets
    // the same nullptr an exhausted window would give, which its failure path
    // already handles.
    if (!READY) {
        log::error("vmalloc: called before the window was initialised");
        return nullptr;
    }
    if (NR_BUSY == MAX_LIVE_RANGES) {
        return nullptr;
    }

    // Not Lazy: a kernel address cannot be faulted in -- Thread::Current::fault()
    // answers NotFound for anything above the split -- so every page a kernel
    // mapping covers has to be present before the first access.
    ustl::Arc<ours::mm::VmObjectPaged> vmo;
    auto status = ours::mm::VmObjectPaged::create(gaf, rounded, ours::mm::VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return nullptr;
    }
    vmo->set_name("vmalloc");

    // map_with_vmo(), not create_mapping(). create_mapping() builds the mapping
    // object and stops there, accepting a VmMapOption it never acts on, so the
    // range would come back live with no page tables behind it. Committing the
    // VMO, installing the entries, and destroying the half-built mapping when
    // either fails all live in map_with_vmo().
    auto mapping = WINDOW->map_with_vmo(ours::mm::VmPlacement::anywhere(), rounded, 0, MMU_FLAGS,
                                        ours::mm::VmaFlags::PermMask, vmo,
                                        ours::mm::VmMapOption::Populate, "vmalloc");
    if (mapping.is_err()) {
        // The VMO's last reference goes with this return, and ~VmCowPages
        // gives its pages back -- which is only true since that destructor
        // stopped being `= default`.
        return nullptr;
    }

    auto const base = mapping.unwrap()->base();
    BUSY[NR_BUSY++] = ustl::move(mapping.unwrap());
    return reinterpret_cast<void *>(base);
}

auto vfree(void *const pointer) -> void {
    if (pointer == nullptr) {
        return;
    }

    auto const base = ours::VirtAddr(pointer);
    DEBUG_ASSERT(is_vmalloc_addr(pointer), "vfree() on an address outside the vmalloc window");

    Guard guard{LOCK};

    auto found = MAX_LIVE_RANGES;
    for (auto index = usize{0}; index < NR_BUSY; ++index) {
        if (BUSY[index]->base() == base) {
            found = index;
            break;
        }
    }
    if (found == MAX_LIVE_RANGES) {
        // An interior pointer, a double free, or a pointer this allocator never
        // produced. None can be served, and guessing at a length would unmap
        // memory belonging to someone else.
        DEBUG_ASSERT(false, "vfree() on an address this allocator did not hand out");
        return;
    }

    auto const size = BUSY[found]->size();

    // Swap with the last live record rather than shifting: the table is
    // searched linearly and has no order to preserve.
    //
    // Both lines are needed, including when |found| *is* the last record and
    // the first one becomes a self-move. ustl::Rc's move assignment guards
    // that with `if (this != &other)`, so the self-move does nothing and it is
    // the clear on the next line that releases the reference; in the ordinary
    // case the move releases it and the clear is the no-op. Either way exactly
    // one release happens, and it happens here rather than whenever the slot
    // is next overwritten.
    BUSY[found] = ustl::move(BUSY[--NR_BUSY]);
    BUSY[NR_BUSY] = {};

    // VmArea::unmap() is what retires a mapping: it clears the page-table
    // entries, unlinks the mapping from its VMO and from this area's set, and
    // drops the reference the set was holding. With this table's reference gone
    // too, the mapping dies, the VMO follows, and ~VmCowPages returns the pages.
    auto const status = WINDOW->unmap(base, size);
    if (Status::Ok != status) {
        log::error("vmalloc: could not unmap [{:X}, {:X}): {}", base, base + size,
                   ours::to_string(status));
    }
}

auto vmalloc_live_ranges() -> usize {
    Guard guard{LOCK};
    return NR_BUSY;
}

/// Proves the allocator works on the machine it was built for, once, at boot.
///
/// Every case here needs an MMU underneath -- the pages, the page tables, the
/// teardown -- which is why it runs on the target rather than as a host test.
///
/// The last check is the one that matters most. Every other case fails loudly;
/// a leak does not, and a teardown path is the easiest place to grow one.
auto vmalloc_self_test() -> void {
    auto const before = vmalloc_live_ranges();
    auto failed = usize{0};

    auto const check = [&failed](bool const ok, char const *const what) {
        if (!ok) {
            log::error("vmalloc: FAILED {}", what);
            ++failed;
        }
    };

    // A single page, written end to end: a mapping that is short or carries
    // the wrong permissions faults here rather than returning.
    auto *one = static_cast<u8 *>(vmalloc(PAGE_SIZE, ours::mm::GAF_KERNEL));
    check(one != nullptr, "one-page allocation");
    if (one) {
        check(is_vmalloc_addr(one), "the result is inside the window");
        check(ours::VirtAddr(one) % PAGE_SIZE == 0, "the result is page aligned");
        for (auto index = usize{0}; index < PAGE_SIZE; ++index) {
            one[index] = u8(index);
        }
        check(one[0] == 0 && one[PAGE_SIZE - 1] == u8(PAGE_SIZE - 1),
              "the page holds what was written");
    }

    // A size that is neither a page multiple nor a power of two. The round-up
    // is where an off-by-one hides, and get_frames_n() -- the hand-rolled path
    // this replaces -- would have quietly rounded 9 KiB up to 16.
    auto *odd = static_cast<u8 *>(vmalloc(2 * PAGE_SIZE + 1, ours::mm::GAF_KERNEL));
    check(odd != nullptr, "odd-sized allocation");
    if (odd) {
        odd[0] = 1;
        odd[2 * PAGE_SIZE] = 2;
        check(odd[0] == 1 && odd[2 * PAGE_SIZE] == 2, "the last page of an odd size is mapped");
    }

    check(!is_vmalloc_addr(&NR_BUSY),
          "a kernel address outside the window is not mistaken for one");
    check(vmalloc(0, ours::mm::GAF_KERNEL) == nullptr, "a zero-sized request is refused");
    check(vmalloc(VMALLOC_SIZE + PAGE_SIZE, ours::mm::GAF_KERNEL) == nullptr,
          "a request larger than the window is refused");

    // The routing, end to end: a request past the size-class ladder has to come
    // back with a window address, a small one has to stay in the physmap, and
    // both have to survive the trip home through the same `delete`, which has
    // only the address to go on.
    auto *big = new (ours::mm::GAF_KERNEL) u8[2 * KMALLOC_MAX_SIZE];
    check(big != nullptr, "new above the kmalloc ladder");
    if (big) {
        check(is_vmalloc_addr(big), "a large new is served by vmalloc");
        big[0] = 0x5A;
        big[2 * KMALLOC_MAX_SIZE - 1] = 0xA5;
        check(big[0] == 0x5A && big[2 * KMALLOC_MAX_SIZE - 1] == 0xA5,
              "a large new is writable end to end");
    }

    auto *small = new (ours::mm::GAF_KERNEL) u8[64];
    check(small != nullptr, "new below the kmalloc ladder");
    check(small == nullptr || !is_vmalloc_addr(small), "a small new is still served by kmalloc");

    // Impossible, and it must come back as a null pointer rather than as a
    // fault. That a non-trivial type's constructor would also be skipped is the
    // language rule the noexcept on those overloads buys; the static_assert in
    // new.cpp is what holds it, since it cannot be observed from here without
    // arranging a failure for a type that has one.
    //
    // One page *past* the window, not the window's own size. Exactly
    // VMALLOC_SIZE is a legal request that this machine cannot back, and
    // finding that out means committing up to a million pages before the
    // allocator runs dry. While VmObjectPaged::commit_range() still recommitted
    // the whole range once per page, a test written that way took the boot
    // from three seconds to over seven minutes before faulting; the loop is
    // gone, but a boot-time self test still has no business exhausting the
    // machine. The request under test here is the one that is refused by
    // inspection.
    auto *impossible = new (ours::mm::GAF_KERNEL) u8[VMALLOC_SIZE + PAGE_SIZE];
    check(impossible == nullptr, "an impossible new returns null");

    delete[] small;
    delete[] big;

    vfree(odd);
    vfree(one);
    vfree(nullptr);

    check(vmalloc_live_ranges() == before, "every range was retired");

    // And the physical pages come back with them.
    //
    // The count above only proves the bookkeeping is empty; this exercises the
    // path that returns the memory. It is a smoke test at this size, and
    // deliberately stays one.
    //
    // Turning it into a proof was tried and abandoned. At CHURN_ROUNDS = 8192
    // it churns half a gigabyte through a 512 MiB guest, which the fixed
    // kernel completes; built against the `~VmCowPages() = default` this
    // series replaced, the same run reached ~1975 rounds -- about 494 MiB
    // leaked -- and was still going when the boot was cut off. It would not
    // have reported anything useful past that point either: mm's
    // alloc_frames_slowpath() spins forever on exhaustion for a GAF_KERNEL
    // request rather than returning null (docs/repairs/260908/heap.md), so the
    // null branch below is unreachable in exactly the case it was written for.
    // A leak of this kind hangs the machine; it does not fail a check.
    //
    // So the standing value is small enough to cost a fraction of a second,
    // and the real regression barrier for ~VmCowPages is the destructor's own
    // code rather than a runtime probe.
    constexpr auto CHURN_PAGES = usize{64};
    constexpr auto CHURN_ROUNDS = usize{128};
    for (auto round = usize{0}; round < CHURN_ROUNDS; ++round) {
        auto *block = vmalloc(CHURN_PAGES * PAGE_SIZE, ours::mm::GAF_KERNEL);
        if (block == nullptr) {
            log::error("vmalloc: FAILED pages are not returned (round {} of {})", round,
                       CHURN_ROUNDS);
            ++failed;
            break;
        }
        // Touched, so a mapping that is merely reserved rather than backed
        // shows up here rather than at some later caller.
        static_cast<u8 *>(block)[CHURN_PAGES * PAGE_SIZE - 1] = u8(round);
        vfree(block);
    }

    if (failed) {
        log::error("vmalloc: self test failed {} check(s)", failed);
    } else {
        log::info("vmalloc: self test passed");
    }
}

} // namespace heap
