#include <ours/lockdep.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>

#include <cstring>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/option.hpp>

using ustl::round_down;
using ustl::round_up;

namespace ours::mm {
using VmoPagedCache = ObjectCache::Instance<VmObjectPaged>;

VmObjectPaged::VmObjectPaged(VmoFlags vmof, ustl::Arc<VmCowPages> cowpages, u64 const cow_offset,
                             usize const size)
    : Base(gktl::magic<Self>(), Type::Paged, vmof, cowpages->lock())
    , m_cow_pages(ustl::move(cowpages))
    , m_cow_offset(cow_offset)
    , m_size(size)
    , m_attachment(this) {
    // Registered with the pages, under their lock, so that an operation on the
    // pages can reach this object's mappings. Not yet reachable by anyone else,
    // so nothing can be missed between the two.
    Guard guard{m_cow_pages->lock()};
    m_cow_pages->lock().assert_held();
    m_cow_pages->attach_locked(m_attachment);
}

/// Off the pages' list before anything of this object is gone, under the same
/// lock: an operation on the pages that is walking the list holds it, so it
/// never sees an object half destroyed. The reference to the pages is released
/// after this body, outside the lock, as the last reference to a node must be.
VmObjectPaged::~VmObjectPaged() {
    Guard guard{m_cow_pages->lock()};
    m_cow_pages->lock().assert_held();
    m_cow_pages->detach_locked(m_attachment);
}

auto VmObjectPaged::operate_cow_range_locked(u64 const cow_offset, u64 const len, RangeOp const op)
    -> void {
    // This object shows [m_cow_offset, m_cow_offset + m_size) of the pages.
    u64 end = cow_offset + len;
    if (len > ~u64{0} - cow_offset) {
        end = ~u64{0};
    }
    u64 const first = std::max<u64>(cow_offset, m_cow_offset);
    u64 const last = std::min<u64>(end, m_cow_offset + m_size);
    if (first >= last) {
        return;
    }
    operate_range_locked(first - m_cow_offset, last - first, op);
}

auto VmObjectPaged::create(Gaf alloc_flags, usize size, VmoFlags vmof,
                           ustl::Arc<VmObjectPaged> *out) -> Status {
    // Check vmof
    auto cow_flags = VmCowPages::Flags::None;
    if (!!(vmof & VmoFlags::LargePages)) {
        cow_flags |= VmCowPages::Flags::LargeBlocks;
    }
    if (!!(vmof & VmoFlags::Pinned)) {
        cow_flags |= VmCowPages::Flags::Pinned;
    }
    ustl::Arc<VmCowPages> cow_pages;
    auto status = VmCowPages::create(alloc_flags, size, cow_flags, &cow_pages);
    if (Status::Ok != status) {
        return status;
    }

    auto vmo = VmoPagedCache::get(ALLOC_KERNEL, vmof, ustl::move(cow_pages), 0, size);
    if (!vmo) {
        return Status::OutOfMem;
    }
    // Adopted before the commit, not after it. If the commit fails part way,
    // dropping this reference is what gives back the pages it did get
    // (~VmCowPages frees every page it owns); adopted afterwards, a failed
    // commit would leave nobody holding the object or its pages.
    auto vmo_ref = ustl::Arc<Self>::adopt(vmo);

    // A non-lazy VMO has every page it will ever need committed here, so that
    // no access to it can fault. That is the whole promise of the flag, and it
    // is why the status matters: a kernel stack is Pinned, and a stack page
    // that is missing turns the first push that reaches it into a page fault
    // on a kernel address -- which page_fault() refuses and the kernel dies
    // of. The status used to be thrown away, and create() answered Ok for a
    // VMO with holes in it.
    //
    // An empty VMO has nothing to commit and is skipped, not handed to
    // commit_range(): a Pinned commit of zero bytes is InvArgs there, because
    // pinning nothing is a caller error. VmArea::reserve() makes exactly such a
    // VMO for every reservation it records -- and while the commit status was
    // ignored, nobody saw that its commit had always failed.
    if (!(vmof & VmoFlags::Lazy) && size != 0) {
        CommitOptions options{};
        if (!!(vmof & VmoFlags::Pinned)) {
            options |= CommitOptions::Pin;
        }

        auto const commit_status = vmo_ref->commit_range(0, size, options);
        if (Status::Ok != commit_status) {
            return commit_status;
        }
    }
    *out = ustl::move(vmo_ref);

    return Status::Ok;
}

auto VmObjectPaged::create_external(ustl::Arc<PageSource> source, Gaf const alloc_flags,
                                    usize const size, VmoFlags const vmof,
                                    ustl::Arc<VmObjectPaged> *const out) -> Status {
    DEBUG_ASSERT(source);

    if (!!(vmof & VmoFlags::Pinned) || !ustl::is_aligned(size, PAGE_SIZE)) {
        source->close();
        return Status::InvArgs;
    }

    // A copy is handed over and this one kept until the pages exist: a source
    // nobody closed must not be freed, and if the pages cannot be made, this
    // reference is the one left to close it with. Once they exist they own
    // the source, and close it when they die.
    ustl::Arc<VmCowPages> cow_pages;
    if (auto const status = VmCowPages::create_external(source, alloc_flags, size, &cow_pages);
        Status::Ok != status) {
        source->close();
        return status;
    }

    auto *const vmo = VmoPagedCache::get(ALLOC_KERNEL, vmof, ustl::move(cow_pages), 0, size);
    if (!vmo) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(vmo);

    return Status::Ok;
}

auto VmObjectPaged::create_contiguous(Gaf const alloc_flags, usize const size, usize alignment_log2,
                                      Flags const vmof, ustl::Arc<VmObjectPaged> *const out)
    -> Status {
    // A block is taken in one go, with no lock held across a wait to give it up
    // for: CanWait is for callers that sleep until a frame is free, and this
    // one answers NoMemory instead.
    DEBUG_ASSERT(!(alloc_flags & Gaf::CanWait), "create_contiguous() does not wait for memory");
    if (size == 0 || !!(vmof & (VmoFlags::Lazy | VmoFlags::LargePages))) {
        return Status::InvArgs;
    }
    if (alignment_log2 == 0) {
        alignment_log2 = PAGE_SHIFT;
    }
    // A shift of 64 or more is not an alignment, and one below the page's is
    // less than a page: neither is asked for by a caller that meant it.
    if (alignment_log2 < PAGE_SHIFT || alignment_log2 >= 64) {
        return Status::InvArgs;
    }
    if (size > MAX_SIZE) {
        return Status::OutOfRange;
    }

    auto const rounded = ustl::round_up(size, PAGE_SIZE);
    auto const nr_pages = rounded / PAGE_SIZE;
    // A block is aligned to its own size, so one big enough for the alignment
    // is aligned as asked; the order is the larger of what the size and the
    // alignment need. Compared before it is shifted by: an alignment that
    // cannot be met by any block must not overflow a shift.
    auto const align_order = alignment_log2 - PAGE_SHIFT;
    auto const order = std::max(get_order(nr_pages), align_order);
    if (order > MAX_FRAME_ORDER) {
        return Status::NoMemory;
    }

    ustl::Arc<VmCowPages> cow_pages;
    if (auto const status =
            VmCowPages::create(alloc_flags, rounded, VmCowPages::Flags::Pinned, &cow_pages);
        Status::Ok != status) {
        return status;
    }

    // Zero, as every page a program can reach is: a device's buffer is not the
    // place for what the last owner of the frames left in them.
    auto const allocated = alloc_frames(alloc_flags | Gaf::Zero, order);
    if (!allocated) {
        return Status::NoMemory;
    }
    auto *const block = *allocated;

    // Each frame is its own from here, as if it had been allocated alone (the
    // way fill_block_locked() treats a large block): the tail goes back now,
    // one page at a time, and the pages kept go into the node and are freed
    // one at a time with it.
    auto const nr_frames = usize{1} << order;
    for (usize index = 0; index < nr_frames; ++index) {
        block[index].set_order(0);
    }
    for (usize index = nr_pages; index < nr_frames; ++index) {
        free_frames(&block[index], 0);
    }

    auto status = Status::Ok;
    {
        Guard guard{cow_pages->lock()};
        cow_pages->lock().assert_held();
        for (usize index = 0; index < nr_pages; ++index) {
            status = cow_pages->add_new_page_locked(index, &block[index]);
            if (Status::Ok != status) {
                // What went in is the node's and goes with it; what did not, and
                // this one, is given back here.
                for (usize rest = index; rest < nr_pages; ++rest) {
                    free_frames(&block[rest], 0);
                }
                break;
            }
        }
    }
    if (Status::Ok != status) {
        return status;
    }

    // Constructed after the lock is released: the constructor takes the same
    // mutex to attach, and it is not recursive.
    auto *const vmo = VmoPagedCache::get(ALLOC_KERNEL, vmof | VmoFlags::Pinned,
                                         ustl::move(cow_pages), 0, rounded);
    if (!vmo) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(vmo);
    return Status::Ok;
}

auto VmObjectPaged::create_from_wired_pages(Gaf const alloc_flags, PhysAddr const base,
                                            usize const size, ustl::Arc<VmObjectPaged> *const out)
    -> Status {
    if (size == 0 || !ustl::is_aligned(base, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    ustl::Arc<VmCowPages> cow_pages;
    if (auto const status = VmCowPages::create(alloc_flags, size, &cow_pages);
        Status::Ok != status) {
        return status;
    }

    // Under the pages' lock, though nobody else can reach them yet: the
    // insert path is written for it. Each frame is unwired first and added
    // second; a failure of either is fatal.
    {
        Guard guard{cow_pages->lock()};
        cow_pages->lock().assert_held();
        for (usize index = 0; index < size / PAGE_SIZE; ++index) {
            auto const phys = base + index * PAGE_SIZE;
            auto *const page = phys_to_frame(phys);
            ASSERT(!!(page->flags() & page::Flags::Reserved),
                   "frame {:#x} backing a wired VMO is not reserved", phys);
            unwire_frame(page);
            auto const status = cow_pages->add_new_page_locked(index, page);
            ASSERT(Status::Ok == status, "adding wired frame {:#x} as page {} of {}: {}", phys,
                   index, size / PAGE_SIZE, to_string(status));
        }
    }

    // Constructed after the lock is released: the constructor takes the same
    // mutex to attach, and it is not recursive.
    auto *const vmo = VmoPagedCache::get(ALLOC_KERNEL, VmoFlags{}, ustl::move(cow_pages), 0, size);
    if (!vmo) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(vmo);
    return Status::Ok;
}

/// No lock is taken: everything read here -- the window and the pages it
/// sits on -- is fixed when the parent is built, and the child only adds a
/// reference to the pages. The lock would be needed to link the child into
/// the parent's list of children, which is not kept (nothing yet walks it).
///
/// The child keeps the parent's flags. For a Pinned parent every page of the
/// window is already committed, and stays so; for a Lazy one the child
/// commits into the shared tree as it is touched, which the parent then sees.
auto VmObjectPaged::create_slice(u64 const offset, u64 const size,
                                 ustl::Arc<VmObjectPaged> *const out) -> Status {
    m_canary.verify<Self>();

    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }
    // Wholly inside this object, written so that it cannot wrap.
    if (size > m_size || offset > m_size - size) {
        return Status::InvArgs;
    }

    auto child =
        VmoPagedCache::get(ALLOC_KERNEL, m_flags, m_cow_pages, m_cow_offset + offset, usize(size));
    if (!child) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(child);
    return Status::Ok;
}

auto VmObjectPaged::create_clone(u64 const offset, u64 const size,
                                 ustl::Arc<VmObjectPaged> *const out) -> Status {
    m_canary.verify<Self>();

    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }
    if (offset > m_size || size > MAX_SIZE) {
        return Status::InvArgs;
    }
    if (!!(m_flags & Flags::Pinned)) {
        return Status::BadState;
    }

    ustl::Arc<VmCowPages> pages;
    {
        Guard guard{m_lock};
        assert_cow_pages_locked();
        // In the pages' bytes, and no further than the end of this object's
        // window of them: what lies past a slice is the parent's, not the
        // slice's to show.
        if (auto const status = m_cow_pages->create_clone_locked(m_cow_offset + offset, size,
                                                                 m_cow_offset + m_size, &pages);
            Status::Ok != status) {
            return status;
        }
    }

    // The object is made outside the lock: its constructor registers it with the
    // pages, under that lock. If it cannot be made the clone goes with |pages|,
    // and this object's pages stay as the clone left them, which is consistent:
    // the hidden node is merged away when the clone that hung under it is gone.
    auto child = VmoPagedCache::get(ALLOC_KERNEL, m_flags, ustl::move(pages), u64{0}, usize(size));
    if (!child) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(child);
    return Status::Ok;
}

auto VmObjectPaged::commit_range_internal(usize const offset, usize const size,
                                          VmCowPages::CommitMode const mode) -> Status {
    MultiPageRequest request;
    usize done = 0;
    auto status = Status::Ok;
    while (done < size) {
        {
            Guard guard{m_lock};
            // Recheck the whole range after each wait, with the lock held.
            if (size > m_size || offset > m_size - size) {
                status = Status::OutOfRange;
                break;
            }

            usize nr_done = 0;
            assert_cow_pages_locked();
            status = m_cow_pages->commit_range_locked(m_cow_offset + offset + done, size - done,
                                                      mode, request, nr_done);
            done += nr_done * PAGE_SIZE;
            if (Status::ShouldWait != status) {
                break;
            }
        }

        status = request.wait(true);
        if (Status::Ok != status) {
            break;
        }
    }

    if (mode == VmCowPages::CommitMode::Pin && Status::Ok != status && done != 0) {
        // A failed pin releases this call's pins. The materialized pages stay.
        Guard guard{m_lock};
        assert_cow_pages_locked();
        [[maybe_unused]]
        auto const undone = m_cow_pages->unpin_range_locked(m_cow_offset + offset, done);
        DEBUG_ASSERT(Status::Ok == undone, "pages this call pinned were not pinned");
    }
    return status;
}

auto VmObjectPaged::commit_range(usize const offset, usize const size, CommitOptions const option)
    -> Status {
    m_canary.verify<Self>();

    log::trace("VMO Commit Action: [ofs: {:X}, size: {:X}]", offset, size);
    if (size == 0) {
        return !!(option & CommitOptions::Pin) ? Status::InvArgs : Status::Ok;
    }
    return commit_range_internal(offset, size, VmCowPages::CommitMode::Owned);
}

auto VmObjectPaged::decommit(usize offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::pin_range(usize const offset, usize const size) -> Status {
    m_canary.verify<Self>();

    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE) || !size) {
        return Status::InvArgs;
    }
    return commit_range_internal(offset, size, VmCowPages::CommitMode::Pin);
}

auto VmObjectPaged::unpin_range(usize const offset, usize const size) -> Status {
    m_canary.verify<Self>();

    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE) || !size) {
        return Status::InvArgs;
    }

    Guard guard{m_lock};
    if (size > m_size || offset > m_size - size) {
        return Status::OutOfRange;
    }
    assert_cow_pages_locked();
    return m_cow_pages->unpin_range_locked(m_cow_offset + offset, size);
}

auto VmObjectPaged::lookup_contiguous(usize const offset, usize const size, PhysAddr *const out)
    -> Status {
    m_canary.verify<Self>();

    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE) || !size) {
        return Status::InvArgs;
    }

    Guard guard{m_lock};
    if (size > m_size || offset > m_size - size) {
        return Status::OutOfRange;
    }

    assert_cow_pages_locked();
    auto cursor = m_cow_pages->make_cursor(m_cow_offset + offset, size);
    // Looking is not using: the answer is about where the pages are, and an
    // access through the physmap that this is not.
    cursor.disable_mark_accessed();

    auto first = PhysAddr{0};
    for (usize at = 0; at < size; at += PAGE_SIZE) {
        auto const found = cursor.maybe_page();
        if (found.is_none()) {
            return Status::BadState;
        }
        auto const frame = frame_to_phys(found.value().page);
        if (at == 0) {
            first = frame;
        } else if (frame != first + at) {
            return Status::BadState;
        }
    }
    *out = first;
    return Status::Ok;
}

auto VmObjectPaged::take_pages(usize const offset, usize const size, VmPageSpliceList *const pages)
    -> Status {
    m_canary.verify<Self>();

    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE) || !size ||
        !pages->is_fresh() || pages->nr_pages() != size / PAGE_SIZE) {
        return Status::InvArgs;
    }

    Guard guard{m_lock};
    // Written so that it cannot wrap, as commit_range_internal() is.
    if (size > m_size || offset > m_size - size) {
        return Status::OutOfRange;
    }
    // Checked here and not by the node, as the clone checks it: pinning is the
    // object's (VmoFlags::Pinned), and the kernel's mappings of a pinned
    // object rely on its pages staying where they are.
    if (!!(m_flags & Flags::Pinned)) {
        return Status::BadState;
    }

    assert_cow_pages_locked();
    return m_cow_pages->take_pages_locked(m_cow_offset + offset, size, pages);
}

auto VmObjectPaged::supply_pages(usize const offset, usize const size,
                                 VmPageSpliceList *const pages, SupplyOption const options)
    -> Status {
    m_canary.verify<Self>();

    if (!!(options & SupplyOption::TransferData)) {
        return Status::NotSupported;
    }
    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE) || !size ||
        !pages->is_unread() || pages->nr_pages() != size / PAGE_SIZE) {
        return Status::InvArgs;
    }

    Guard guard{m_lock};
    // Written so that it cannot wrap, as commit_range_internal() is.
    if (size > m_size || offset > m_size - size) {
        return Status::OutOfRange;
    }

    assert_cow_pages_locked();
    return m_cow_pages->supply_pages_locked(m_cow_offset + offset, size, pages);
}

auto VmObjectPaged::detach_source() -> void {
    m_canary.verify<Self>();

    Guard guard{m_lock};
    assert_cow_pages_locked();
    m_cow_pages->detach_source_locked();
}

auto VmObjectPaged::source_owner_id() const -> ustl::Option<u64> {
    return m_cow_pages->page_source_owner_id();
}

/// One walk over the pages [offset, offset + size) touches, handing |copy| the
/// physmap address of each and the slice of the caller's buffer that belongs
/// to it.
///
/// |copy| answers with a UserCopyCaptureResult. A kernel-buffer copy (read(),
/// write()) cannot fault and always answers Ok. A user-buffer copy
/// (read_user(), write_user()) can, and it runs under m_lock, so the fault
/// must not be resolved in place: resolving takes the address space's lock,
/// which would put it inside this VMO's -- the reverse of the order the page
/// fault path takes them in -- and would deadlock outright if the user buffer
/// is a mapping of this very VMO. It uses the capturing copies instead and
/// hands the fault back. This function then lets go of m_lock, resolves the
/// fault with soft_fault() where sleeping and the address space's lock are
/// both allowed, takes m_lock again, and carries on from the first byte not
/// yet copied.
///
/// Carrying on means looking the page up again, never reusing what was found
/// before the lock went down. In that window the VMO is anybody's: the page at
/// this offset may have been replaced, or decommitted and its memory handed to
/// something else, so a pointer kept across the drop would read or write
/// memory that no longer belongs to this VMO. What survives is |done|, which is
/// this function's own progress and says nothing about the VMO's state; the
/// cursor is rebuilt from it every time the lock is retaken.
///
/// A page that is not there yet is the other reason to let go of m_lock. For a
/// VMO with a page source, require_page() raises a request and answers
/// ShouldWait; the provider supplies the page under this same lock, so the
/// wait happens with it released, and the walk carries on from |done| as it
/// does after a fault. The request is declared outside the locked block for
/// that reason: it is raised under the lock and waited on after it.
///
/// The shape: range check, a lookup cursor over the page range, require_page()
/// per page, copy through the physmap, and on a captured fault drop the lock,
/// soft_fault() and retry; on a page request drop the lock, wait and retry.
/// There is no partial-length return: a call that fails here fails, and says
/// so.
///
/// m_lock is a sleeping lock, not a spin lock, so a long copy holds up other
/// users of this VMO but never a CPU with interrupts off.
auto VmObjectPaged::copy_page_by_page(usize const offset, usize const size, bool const will_write,
                                      CopyPage const &copy) -> Status {
    m_canary.verify<Self>();

    if (!size) {
        return Status::Ok;
    }

    usize done = 0;
    // The last fault resolved, and how far the copy had got when it was taken;
    // see the livelock check below.
    ustl::Option<VirtAddr> retried_at;
    usize retried_done = 0;
    // Raised under the lock, waited on outside it; see above.
    MultiPageRequest request;

    while (done < size) {
        ustl::Option<UserCopyFault> fault;
        auto must_wait = false;
        {
            Guard guard{m_lock};
            // Checked before anything is copied, and written so that it cannot
            // wrap, as commit_range_internal() is. A range that runs off the end
            // copies nothing rather than a prefix: no caller here wants a short
            // read, and there is no option to trim the length instead.
            // Rechecked each time the lock is retaken, because the size is
            // only known to be what it was while the lock is held.
            if (size > m_size || offset > m_size - size) {
                return Status::OutOfRange;
            }

            // The cursor serves whole pages, so it is made for the page-aligned
            // span from the first byte still to copy to the end of the range;
            // the copy below cuts each page back down to the bytes asked for.
            auto const first_page = round_down(offset + done, PAGE_SIZE);
            auto const end_page = round_up(offset + size, PAGE_SIZE);
            assert_cow_pages_locked();
            auto cursor =
                m_cow_pages->make_cursor(m_cow_offset + first_page, end_page - first_page);

            while (done < size) {
                auto const at = offset + done;
                auto const in_page = at - round_down(at, PAGE_SIZE);
                auto const chunk = std::min<usize>(size - done, PAGE_SIZE - in_page);

                // A reader is content with whatever is there and a writer needs
                // a page of its own -- the same choice the fault path makes. For
                // an anonymous VMO both commit a zeroed page where there was
                // none, so a read of a never-written page reads zero (see
                // VmCowPages::Cursor).
                auto page = cursor.require_page(will_write, 1, &request);
                if (!page) {
                    if (Status::ShouldWait == page.unwrap_err()) {
                        must_wait = true;
                        break;
                    }
                    return page.unwrap_err();
                }

                // A write asked for an owned page, which is always writable.
                DEBUG_ASSERT(!will_write || page->writable);
                auto *const base = PhysMap::phys_to_virt<u8>(frame_to_phys(page->page));
                auto const result = copy(base + in_page, done, chunk);
                if (result.fault.is_some()) {
                    fault = result.fault;
                    break;
                }
                if (result.status != Status::Ok) {
                    return result.status;
                }
                done += chunk;
            }
        }

        // Suspendable, as a fault taken from user mode is: the copy runs for a
        // system call, or for a kernel thread that asked for it, and either may
        // be stopped while the provider takes its time. A wait that ends
        // without the pages (the source failed them, or detached, or the
        // thread was killed) fails the copy with its status.
        if (must_wait) {
            if (auto const status = request.wait(true); Status::Ok != status) {
                return status;
            }
            continue;
        }

        if (fault.is_none()) {
            break;
        }

        // A fault that comes back at the same address with no progress since
        // the last one was resolved means soft_fault() said the page was fine
        // and the copy still cannot touch it. Going round again would spin
        // here for ever with nothing to say why, which is worse than failing.
        // A race can land here too -- the page resolved, then unmapped again
        // before the lock came back -- and for that InvArgs is the right answer
        // anyway: the program took away its own buffer.
        if (retried_at.is_some() && *retried_at == fault->va && retried_done == done) {
            log::warn("VMO user copy: fault at {:#x} persists after it was resolved", fault->va);
            return Status::InvArgs;
        }
        retried_at = ustl::some(fault->va);
        retried_done = done;

        // Outside the lock: may sleep, and takes the address space's lock. A
        // fault that cannot be resolved is a user buffer that cannot take or
        // give the bytes, which user_copy answers InvArgs to everywhere.
        if (task::Thread::Current::soft_fault(fault->va, fault->cause) != Status::Ok) {
            return Status::InvArgs;
        }
    }

    return Status::Ok;
}

/// The window is clipped to this object first, as make_cursor() checks it:
/// the page tree is a slice's parent's too, and its pages past the slice's
/// end are not the slice's to count.
auto VmObjectPaged::committed_pages(u64 const offset, u64 const len) -> usize {
    if (offset >= m_size) {
        return 0;
    }
    auto const clipped = std::min<u64>(len, m_size - offset);
    Guard guard{m_cow_pages->lock()};
    m_cow_pages->lock().assert_held();
    return m_cow_pages->nr_pages_in_locked(m_cow_offset + offset, clipped);
}

auto VmObjectPaged::read(void *const out, usize const offset, usize const size) -> Status {
    auto *const dst = static_cast<u8 *>(out);
    return copy_page_by_page(offset, size, false,
                             [dst](u8 const *const page, usize const done, usize const chunk) {
                                 std::memcpy(dst + done, page, chunk);
                                 return UserCopyCaptureResult{.status = Status::Ok, .fault = {}};
                             });
}

auto VmObjectPaged::write(void const *const in, usize const offset, usize const size) -> Status {
    auto const *const src = static_cast<u8 const *>(in);
    return copy_page_by_page(offset, size, true,
                             [src](u8 *const page, usize const done, usize const chunk) {
                                 std::memcpy(page, src + done, chunk);
                                 return UserCopyCaptureResult{.status = Status::Ok, .fault = {}};
                             });
}

auto VmObjectPaged::store_u64(usize const offset, u64 const value) -> Status {
    if ((static_cast<u64>(offset) % alignof(u64)) != 0) {
        return Status::InvArgs;
    }
    return copy_page_by_page(
        offset, sizeof(value), true, [value](u8 *const page, usize const done, usize const chunk) {
            if (chunk != sizeof(value) || done != 0) {
                return UserCopyCaptureResult{.status = Status::InvArgs, .fault = {}};
            }
            __atomic_store_n(reinterpret_cast<u64 *>(page), value, __ATOMIC_RELEASE);
            return UserCopyCaptureResult{.status = Status::Ok, .fault = {}};
        });
}

/// The page walk with copy_array_to_user_capture_faults() as its copy: a
/// fault in the user buffer is captured under the lock and resolved with it
/// dropped.
auto VmObjectPaged::read_user(user_copy::UserOutPtr<u8> const out, usize const offset,
                              usize const size) -> Status {
    return copy_page_by_page(
        offset, size, false, [out](u8 const *const page, usize const done, usize const chunk) {
            return out.element_offset(done).copy_array_to_user_capture_faults(page, chunk);
        });
}

/// The page walk with copy_array_from_user_capture_faults() as its copy. A
/// chunk a fault interrupts is copied again whole on the retry; the page is
/// overwritten with the same bytes, so the repetition is harmless.
auto VmObjectPaged::write_user(user_copy::UserInPtr<u8 const> const in, usize const offset,
                               usize const size) -> Status {
    return copy_page_by_page(
        offset, size, true, [in](u8 *const page, usize const done, usize const chunk) {
            return in.element_offset(done).copy_array_from_user_capture_faults(page, chunk);
        });
}

INIT_CODE
static auto init_vmo_paged_cache() -> void {
    auto const status = VmoPagedCache::init("vmo-paged-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmArea");
    }
    log::trace("VmoPagedCache has been created");
}
// Heap, not PlatformEarly: this creates a MemCache, so it needs the object
// caches to be up.
GKTL_INIT_HOOK(VmoPagedCacheInit, init_vmo_paged_cache, gktl::InitLevel::Heap);

} // namespace ours::mm
