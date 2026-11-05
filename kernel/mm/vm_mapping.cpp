#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>

#include <gktl/init_hook.hpp>
#include <ustl/iterator/function.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
using VmMappingCache = ObjectCache::Instance<VmMapping>;

namespace {
    /// The one large leaf a user mapping maps a block of its object's pages
    /// with (.agents/tasks/large-pages): a block's size, 2 MiB, never 1 GiB
    /// (decision D3).
    constexpr usize LARGE_LEAF = VmCowPages::BLOCK_SIZE;
} // namespace

/// Gathers pages to map at consecutive addresses with one set of permissions,
/// and maps them with one map_bulk() call per batch.
/// The caller commits its final batch on success. On early return, staged
/// addresses can be discarded because they hold no references or PTEs.
template <usize MaxNumPages>
class MappingCoalescer {
  public:
    MappingCoalescer(VmMapping *mapping, VirtAddr base, MmuFlags mmuf)
        // Initializer order follows the member declaration order below.
        : m_mapping(mapping), m_va(base), m_nr_pages(0), m_mmuf(mmuf) {}

    /// Both are [[nodiscard]]: a dropped status here is invisible at run time
    /// -- the batch is silently not mapped, and the mapping object is handed
    /// back looking successful. That is exactly how it used to fail.
    [[nodiscard]]
    auto append(PhysAddr phys) -> Status;

    [[nodiscard]]
    auto commit() -> Status;

    /// The permissions of the pages appended from here on. The batch must be
    /// empty, since it maps with one set of permissions: a run of pages whose
    /// permissions change is committed at the change and continues at the next
    /// address with the new ones.
    auto set_flags(MmuFlags mmuf) -> void {
        DEBUG_ASSERT(m_nr_pages == 0);
        m_mmuf = mmuf;
    }

    /// Where the next page appended goes, when the pages between were mapped
    /// some other way -- a block as one leaf. The batch must be empty, since
    /// its pages are contiguous from where it started.
    auto skip_to(VirtAddr const va) -> void {
        DEBUG_ASSERT(m_nr_pages == 0);
        m_va = va;
    }

  private:
    VmMapping *m_mapping;
    VirtAddr m_va;
    PhysAddr m_pa[MaxNumPages];
    usize m_nr_pages;
    MmuFlags m_mmuf;
};

template <usize MaxNumPages>
FORCE_INLINE
auto MappingCoalescer<MaxNumPages>::append(PhysAddr phys) -> Status {
    m_pa[m_nr_pages++] = phys;
    if (m_nr_pages == MaxNumPages) {
        return commit();
    }
    return Status::Ok;
}

template <usize MaxNumPages>
FORCE_INLINE
auto MappingCoalescer<MaxNumPages>::commit() -> Status {
    if (!m_nr_pages) {
        return Status::Ok;
    }

    // An entry already there is an error: a batch goes where nothing is
    // mapped yet. A failure part way is undone, so the batch is mapped whole
    // or not at all.
    auto const status = m_mapping->aspace()->arch_aspace().map_bulk(
        m_va, m_pa, m_nr_pages, m_mmuf,
        ArchMapOptions::ErrorIfExisting | ArchMapOptions::UndoOnFailure, nullptr);
    if (Status::Ok != status) {
        log::error("Failed to map {} pages at {}", m_nr_pages, m_va);
    }

    m_va += m_nr_pages * PAGE_SIZE;
    m_nr_pages = 0;

    // The status the caller acts on. Returning Ok here regardless -- which is
    // what this did -- left map_paged()/map_physical() reporting success for a
    // mapping that was never installed, so the VMM handed out a VmMapping with
    // no page tables behind it and the failure only ever showed up as a log
    // line.
    return status;
}

VmMapping::VmMapping(VmArea *parent, VirtAddr base, usize size, VmaFlags vmaf,
                     ustl::Arc<VmObject> vmo, usize vmo_off, const char *name)
    : Base(gktl::magic<Self>(), base, size, vmaf | VmaFlags::Mapping, parent,
           parent->aspace().as_ptr_mut(), name)
    // Initializer order follows the member declaration order in the header.
    , m_vmo(ustl::move(vmo))
    , m_vmo_ofs(vmo_off)
    , m_protection_ranges() {}

/// Out of line: the copy retains the object, which needs its whole type, and
/// vm_object.hpp includes this header rather than the other way round.
auto VmMapping::vmo() const -> ustl::Arc<VmObject> {
    return m_vmo;
}

auto VmMapping::create(VmArea *parent, VirtAddr base, usize size, VmaFlags vmaf,
                       ustl::Arc<VmObject> vmo, usize vmo_off, MmuFlags mmuf, char const *name,
                       ustl::Arc<Self> *out) -> Status {
    if (!vmo) {
        return Status::InvArgs;
    }

    auto mapping =
        VmMappingCache::get(ALLOC_KERNEL, parent, base, size, vmaf, ustl::move(vmo), vmo_off, name);
    if (!mapping) {
        return Status::OutOfMem;
    }
    auto status = mapping->m_protection_ranges.init(base, size, mmuf);
    if (Status::Ok != status) {
        VmMappingCache::put(mapping);
        return status;
    }
    // Created under the parent area's lock, which is the new mapping's too.
    // Asserted, because the analysis does not follow it through Builder.
    mapping->get_lock().assert_held();
    if (status = mapping->activate(); Status::Ok != status) {
        VmMappingCache::put(mapping);
        return status;
    }

    *out = ustl::Arc<Self>::adopt(mapping);
    return Status::Ok;
}

auto VmMapping::activate() -> Status {
    m_canary.verify<Self>();

    m_parent->get_lock().assert_held();
    if (auto const status = m_parent->link_child_locked(*this); Status::Ok != status) {
        return status;
    }

    // Note when a mapping was added into a VMO's mapping list, the inside counter
    // has not been increased implicitly. It is required to pay attention to manually
    // install and uninstall a mapping, avoiding lifecycle problem.
    //
    // Under the object's lock, which the list belongs to; see
    // VmObject::add_mapping().
    Guard guard{m_vmo->lock()};
    m_vmo->add_mapping(*this);
    mark_active();

    return Status::Ok;
}

auto VmMapping::destroy() -> Status {
    m_canary.verify<Self>();

    // Once only. Everything below undoes something activate() did exactly
    // once, so a second pass would remove an already unlinked node.
    if (!is_active()) {
        return Status::BadState;
    }

    m_aspace->get_lock().assert_held();
    m_aspace->on_mapping_destroyed_locked(*this);

    // Take the mapping out of the page tables first, but do not let a failure
    // here abandon the bookkeeping below. This used to return early, which left
    // the mapping linked into its VMO's mapping list and its parent's subvma
    // set; the last Arc then dropped, ~VmMapping ran on a still-linked node,
    // and ustl's intrusive hook trapped on it. A failed unmap is worth
    // reporting, but a destroyed object left on two live lists is a corrupt
    // list, which is strictly worse.
    auto const status = m_aspace->arch_aspace().unmap(m_base, m_size >> PAGE_SHIFT, {}, nullptr);
    if (Status::Ok != status) {
        log::trace("Failed to destroy Mapping with unmapping {} pages at {}", m_size >> PAGE_SHIFT,
                   m_base);
    }

    // And let go of the VMO. The mapping object can outlive its destruction
    // -- anyone holding an Arc to it keeps it -- and while it kept m_vmo, it
    // kept every page of the object too: a destroyed mapping of a program's
    // image pinned the image. A mapping that maps nothing has no business
    // with a VMO.
    if (m_vmo) {
        {
            Guard guard{m_vmo->lock()};
            m_vmo->remove_mapping(*this);
        }
        m_vmo = {};
    }
    // Unlinking cannot fail: VmAreaOrMapping::destroy() answers only Ok.
    auto _ = Base::destroy();

    // Reported rather than swallowed: the caller learns the page tables may
    // still hold something, while the object itself is safely unregistered.
    return status;
}

FORCE_INLINE
auto VmMapping::map_paged(VmObjectPaged *vmo, VirtAddr base, usize size, MapOptions options)
    -> Status {
    m_canary.verify<Self>();

    DEBUG_ASSERT(ustl::is_aligned(base, PAGE_SIZE));
    DEBUG_ASSERT(ustl::is_aligned(size, PAGE_SIZE));

    bool const commit = options == MapOptions::Commit;
    constexpr static auto MAX_BATCH_PAGES = 32;

    // The cursor is positioned in the VMO's own byte space, not in the address
    // space: |base| is an absolute virtual address, and the offset of that
    // address inside the object is m_vmo_ofs plus its distance from the
    // mapping's own base. Handing |base| straight through -- which is what
    // this did -- worked only because the cursor threw its offset away; now
    // that it keeps one, an absolute kernel address would key the page store
    // at something like index 0xFFFFFF88_00000 and no two mappings of one VMO
    // would ever agree on a slot. map_physical() already converts the same way
    // (`base - m_base`) before calling lookup_range().
    //
    // The object's lock is held from the cursor to the last page table entry,
    // inside the address space lock map() holds: a page found here must still
    // belong to the object when it is mapped. See VmObjectPaged::lock().
    Guard guard{vmo->lock()};
    auto cursor = vmo->make_cursor(m_vmo_ofs + (base - m_base), size);
    if (!cursor) {
        return cursor.unwrap_err();
    }
    // Mapped ahead of use: a page that is then used is used through the entry
    // made here, whose accessed bit records it.
    cursor->disable_mark_accessed();

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    // P0963R3: decomposing in the condition binds the region's fields straight
    // through, so the loop no longer opens by dereferencing an Option just to
    // give them names.
    while (auto [region_base, region_size, mmuf] = enumerator.next()) {
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf);

        // TODO: Request pages in larger batches (e.g. folios) instead of
        // page-by-page. Currently the cursor is asked for one page at a time.
        MultiPageRequest page_request;
        auto batch_mmuf = mmuf;
        for (usize i = 0; i < region_size; i += PAGE_SIZE) {
            // Without Commit, what the object has: a page the node owns comes
            // back writable, one it only sees through an ancestor does not, and
            // a write that comes later faults and asks for a page of its own
            // then. Asking for an owned page here would copy every shared page
            // of the window at the moment of mapping, which is the copying a
            // clone exists to put off; and a page that is not there is left to
            // fault, so that a page source is asked by a fault that can wait
            // for it, never by this, under the address space's lock.
            //
            // With Commit, every page is made the node's own first. A source
            // asked for a page answers ShouldWait, and the map fails with it:
            // nothing that maps with Commit has a source.
            ustl::Option<VmCowPages::Cursor::RequireResult> result;
            if (commit) {
                auto owned = cursor->require_owned_page(1, &page_request);
                if (!owned) {
                    return owned.unwrap_err();
                }
                result = ustl::some(*owned);
            } else {
                result = cursor->maybe_page();
            }

            // A block of the object's pages at a 2 MiB boundary, with the whole
            // of its 2 MiB in this region, goes in as one leaf. The batch so far
            // goes first, so that the coalescer can go on after the block.
            auto const va = region_base + i;
            if (ustl::is_aligned(va, LARGE_LEAF) && region_size - i >= LARGE_LEAF) {
                if (auto const status = coalescer.commit(); Status::Ok != status) {
                    return status;
                }
                if (auto const mapped = map_block_locked(*vmo, va, mmuf)) {
                    if (Status::Ok != mapped->first) {
                        return mapped->first;
                    }
                    coalescer.skip_to(va + LARGE_LEAF);
                    cursor->skip(LARGE_LEAF - PAGE_SIZE);
                    i += LARGE_LEAF - PAGE_SIZE;
                    continue;
                }
            }

            // Nothing to map here: the batch so far goes in, and the next one
            // starts after the page.
            if (result.is_none()) {
                if (auto const status = coalescer.commit(); Status::Ok != status) {
                    return status;
                }
                coalescer.skip_to(va + PAGE_SIZE);
                continue;
            }

            // What the entry may have is what the region allows, less Write for
            // a page that is not the node's to write, as the fault path maps it.
            // The batch is one set of permissions, so a run that changes them is
            // committed at the change and goes on with the new ones.
            auto page_mmuf = mmuf;
            if (!result->writable) {
                page_mmuf &= ~MmuFlags::Writable;
            }
            if (page_mmuf != batch_mmuf) {
                if (auto const status = coalescer.commit(); Status::Ok != status) {
                    return status;
                }
                coalescer.set_flags(page_mmuf);
                batch_mmuf = page_mmuf;
            }

            if (auto const status = coalescer.append(frame_to_phys(result->page));
                Status::Ok != status) {
                return status;
            }
        }
        // Commit those uncovered units in for loop above.
        if (auto const status = coalescer.commit(); Status::Ok != status) {
            return status;
        }
    }

    return Status::Ok;
}

FORCE_INLINE
auto VmMapping::map_physical(VmObjectPhysical *vmo, VirtAddr base, usize size) -> Status {
    m_canary.verify<Self>();

    DEBUG_ASSERT(vmo);
    PhysAddr phys_base;
    // In the object's bytes, as map_paged() converts: the mapping may start
    // anywhere in the object, and |base| is an address. This passed
    // `base - m_base` alone, so a mapping at any object offset but 0 mapped
    // the object's first bytes instead.
    auto status = vmo->lookup_range(m_vmo_ofs + (base - m_base), size, &phys_base);
    if (Status::Ok != status) {
        return status;
    }

    constexpr static auto MAX_BATCH_PAGES = 32;

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [region_base, region_size, mmuf] = enumerator.next()) {
        // The coalescer counts addresses from |region_base|, so the frames are
        // counted from the same place: |phys_base| is |base|'s frame, and
        // every region after the first starts further on.
        auto const region_phys = phys_base + (region_base - base);
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf);
        for (usize offset = 0; offset < region_size; offset += PAGE_SIZE) {
            if (auto const appended = coalescer.append(region_phys + offset);
                Status::Ok != appended) {
                return appended;
            }
        }

        if (auto const committed = coalescer.commit(); Status::Ok != committed) {
            return committed;
        }
    }

    return Status::Ok;
}

/// Do check arguments and dispatch the request to correct sub-routine for different VmObjects.
auto VmMapping::map(VirtAddr base, usize size, MapOptions const mode) -> Status {
    m_canary.verify<Self>();

    if (mode != MapOptions::Resident && mode != MapOptions::Commit) {
        return Status::InvArgs;
    }

    // The address space lock, as protect() and unmap() take it. Without it a
    // map() raced them over the page tables and m_protection_ranges, and a
    // destroy() between the is_active() check below and the mapping itself
    // left pages installed in a range that belonged to nobody. It also puts
    // map_paged() in the lock order the fault path uses: this lock, then the
    // object's.
    Guard guard{get_lock()};

    // A destroyed mapping has no VMO any more (destroy() lets it go), and
    // nothing to map into: its range belongs to nobody now.
    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::round_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvArgs;
    }

    base = ustl::round_down(base, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvArgs;
    }

    if (auto paged = as<VmObjectPaged>(m_vmo.as_ptr_mut())) {
        return map_paged(paged, base, size, mode);
    } else if (auto physical = as<VmObjectPhysical>(m_vmo.as_ptr_mut())) {
        return map_physical(physical, base, size);
    }

    return Status::Ok;
}

auto VmMapping::protect(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    m_canary.verify<Self>();
    Guard guard{get_lock()};

    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::round_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvArgs;
    }

    base = ustl::round_down(base, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvArgs;
    }

    if (!validate_mmuflags(mmuf)) {
        return Status::InvArgs;
    }

    if (is_sealed()) {
        return Status::AccessDenied;
    }

    return protect_locked(base, size, mmuf);
}

auto VmMapping::protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    // The object's lock as well as the address space's, for the whole of it:
    // the record of the permissions and the page tables they are applied to.
    //
    // An operation on the object -- the clone that removes Write from every
    // mapping of the pages it is about to share -- reaches the mappings from
    // the object's side, holding the object's lock and not the address
    // space's, since taking that one inside this one is the wrong way round.
    // What it reads is m_protection_ranges, so whoever writes it has to hold the
    // object's lock too, or the two race on it.
    //
    // And the page tables belong in the same hold, not only the record. Left
    // out, the widening could be interleaved with the clone: the record says
    // Write, the clone removes it from the page tables, and this then applies
    // Write to them, which leaves a page that has become shared writable for
    // good. Aspace lock, then object lock, then the page table's own: the order
    // the fault path takes them in.
    DEBUG_ASSERT(m_vmo);
    Guard guard{m_vmo->lock()};

    auto &arch_aspace = m_aspace->arch_aspace();
    auto status = m_protection_ranges.update(base, size, mmuf);
    if (Status::Ok != status) {
        return status;
    }

    // update() and make_enumerator() both touch the same regions, but region
    // counts per mapping are single digits, so re-walking costs nothing worth
    // entangling the API for. Recorded as won't-do in
    // .agents/tasks/protection-ranges-redesign/DESIGN.md.
    //
    // A region the page tables could not be brought to -- a split of a large
    // page that found no memory, say -- is reported, not swallowed: when the
    // change took a permission away, the caller has to know it did not. The
    // rest are still done, and the first failure is the answer.
    auto first_failure = Status::Ok;
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [region_base, region_size, region_mmuf] = enumerator.next()) {
        if (!(region_mmuf & MmuFlags::PermMask)) {
            // Unmap those regions which has no any permission.
            auto _ = arch_aspace.unmap(region_base, region_size >> PAGE_SHIFT, UnmapOptions::None,
                                       nullptr);
            continue;
        }

        // Narrows the entries to what the region now allows and gives none of them
        // a permission. When the region gained one, the entries are as they were
        // and the first access that needs it faults; page_fault() adds it there,
        // after it has shown the page may have it.
        auto const protected_status =
            arch_aspace.protect(region_base, region_size >> PAGE_SHIFT, region_mmuf);
        if (protected_status != Status::Ok && first_failure == Status::Ok) {
            first_failure = protected_status;
        }
    }

    return first_failure;
}

auto VmMapping::range_change_locked_object(usize const vmo_offset, usize const len,
                                           RangeOp const op) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(m_vmo);
    m_vmo->lock().assert_held();

    // The part of the range this mapping shows, in the object's bytes and then
    // as addresses. |len| may reach the end of the address space, so the end is
    // clamped rather than added.
    auto const end = vmo_offset + std::min(len, ~usize{0} - vmo_offset);
    auto const first = std::max(vmo_offset, m_vmo_ofs);
    auto const last = std::min(end, m_vmo_ofs + m_size);
    if (first >= last) {
        return;
    }
    VirtAddr const base = m_base + (first - m_vmo_ofs);
    usize const size = last - first;
    auto &arch_aspace = m_aspace->arch_aspace();

    if (op == RangeOp::Unmap) {
        // The kernel's own mappings rely on their pages staying where they
        // are, and their objects are made Pinned: an operation that takes pages
        // out of an object refuses a pinned one before it gets here.
        DEBUG_ASSERT(m_aspace->is_user(), "unmapping pages under a kernel mapping of them");

        // The whole range at once, whatever each region allows: a region with
        // no permission has no entries, and unmapping an absent entry is no
        // error. An unmap fails only on part of a 2 MiB leaf with no table set
        // aside to split it into, and a mapping of an object gets a leaf only
        // in a user address space, which sets one aside for every leaf
        // (map_block_locked(), PageTable::Flags::SplitsLargeLeaves).
        auto const status =
            arch_aspace.unmap(base, size >> PAGE_SHIFT, UnmapOptions::None, nullptr);
        ASSERT(Status::Ok == status, "could not unmap pages leaving their object");
        return;
    }

    // Only what can be written is worth touching: a region without Write has
    // page table entries without it, since entries never have more than their
    // region. What is kept is Read and Execute, whatever the region has of them.
    constexpr auto KEEP = MmuFlags::PermMask & ~MmuFlags::Writable;
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [region_base, region_size, region_mmuf] = enumerator.next()) {
        if (!(region_mmuf & MmuFlags::Writable)) {
            continue;
        }
        auto const status = arch_aspace.protect(region_base, region_size >> PAGE_SHIFT, KEEP);
        ASSERT(Status::Ok == status,
               "could not take Write off a mapping of pages about to be shared");
    }
}

auto VmMapping::unmap(VirtAddr base, usize size, UnmapOptions options) -> Status {
    m_canary.verify<Self>();
    Guard guard{get_lock()};

    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::round_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvArgs;
    }

    base = ustl::round_down(base, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvArgs;
    }

    if (is_sealed()) {
        return Status::AccessDenied;
    }

    return unmap_locked(base, size, options);
}

auto VmMapping::unmap_locked(VirtAddr base, usize size, UnmapOptions options) -> Status {
    // The object's lock, for what protect_locked() takes it for: it writes
    // m_protection_ranges, which an operation reaching this mapping from the
    // object's side reads without the address space's lock. See there.
    DEBUG_ASSERT(m_vmo);
    Guard guard{m_vmo->lock()};

    // Allocate protection boundaries before removing any PTE. Once the
    // translations are gone, publishing the hole must be infallible.
    auto const end = base + size;
    if (auto const status = m_protection_ranges.prepare_update(base, end, MmuFlags{});
        Status::Ok != status) {
        return status;
    }

    auto &arch_aspace = m_aspace->arch_aspace();
    if (auto const status = arch_aspace.unmap(base, size >> PAGE_SHIFT, options, nullptr);
        Status::Ok != status) {
        m_protection_ranges.coalesce();
        log::error("Failed to unmap range[{:X}, {:X})", base, end);
        return status;
    }

    m_protection_ranges.apply_update(base, end, MmuFlags{});
    return Status::Ok;
}

/// Resolve one page fault falling inside this mapping.
///
/// Runs with the address space lock already held: VmAspace::page_fault_internal()
/// takes it around finding the mapping and this call, so that the mapping
/// cannot be torn down between the two. Hence no Guard on that lock here --
/// the same division unmap_locked() and protect_locked() already sit on,
/// except that there the public half exists to take the lock, and here the
/// only caller is the fault path. The object's own lock is taken below, inside
/// it, once the fault is known to need a page from a paged VMO.
auto VmMapping::page_fault(VmFault *vmf) -> ustl::Pair<Status, usize> {
    m_canary.verify<Self>();

    DEBUG_ASSERT(vmf, "the fault path used to pass a literal 0 in place of this");

    if (!is_active()) {
        return {Status::BadState, 0};
    }

    VirtAddr const va = ustl::round_down(vmf->va, PAGE_SIZE);
    if (!check_range(va, PAGE_SIZE)) {
        return {Status::InvArgs, 0};
    }

    // What this page may do, taken from the mapping's own record rather than
    // read back out of the page tables. m_protection_ranges is the single
    // writer of the flags -- that is what VmArea::reserve() was changed to
    // respect -- and its regions tile the whole window, so enumerating one
    // page always yields exactly one region.
    auto enumerator = m_protection_ranges.make_enumerator(va, PAGE_SIZE);
    auto const region = enumerator.next();
    DEBUG_ASSERT(region, "the protection regions do not tile this mapping");
    MmuFlags const mmuf = region.mmuf;

    // A region with no permission at all is how unmap() and protect() spell
    // "nothing is mapped here any more". Faulting a page back in would quietly
    // undo them, so this is a refusal, not a miss.
    if (!(mmuf & MmuFlags::PermMask)) {
        return {Status::AccessDenied, 0};
    }

    bool const will_write = !!(vmf->cause & VmFault::Cause::Write);
    if (will_write && !(mmuf & MmuFlags::Writable)) {
        return {Status::AccessDenied, 0};
    }

    // Checked before the page tables are asked, for the same reason the write
    // is: the answer has to be the same whether or not the page is present.
    // The present case is the one that matters. The spurious-fault shortcut
    // below compares only the entry against the region, and a fetch from an
    // NX page whose entry already matches its region -- a user stack --
    // passes that comparison, returns Ok, and the fetch retries into the same
    // fault for ever. The arch handler cannot refuse it instead: after a
    // protect() that added Executable, a fetch through a stale NX entry is
    // spurious, and only this region says which case it is. So an
    // instruction fetch requires Executable of the region ahead of any lookup.
    bool const will_execute = !!(vmf->cause & VmFault::Cause::Instruction);
    if (will_execute && !(mmuf & MmuFlags::Executable)) {
        return {Status::AccessDenied, 0};
    }

    auto &arch_aspace = m_aspace->arch_aspace();

    // Ask the page tables before asking the VMO. A fault can arrive on a page
    // that is already installed: another CPU resolved the same fault while
    // this one waited for the address space lock, or this CPU is still running
    // on a stale TLB entry. Then there is nothing to allocate and nothing to
    // map, and the faulting instruction succeeds on retry.
    //
    // This has to be settled here rather than by handing map() a control that
    // says "overwrite": PageTable::map_pages() ignores MapOptions outright --
    // it says so in a comment -- and the paging engine always rejects an entry
    // that already exists. An unconditional map() therefore reported
    // AlreadyExists for a fault that was in fact already resolved, which the
    // probe written for this change caught on its second fault of one page.
    PhysAddr existing{};
    MmuFlags existing_mmuf{};
    if (Status::Ok == arch_aspace.query(va, &existing, &existing_mmuf)) {
        // What this access needs and the entry lacks. Protection only ever
        // narrows the entries (ArchVmAspace::protect()), so when the mapping is
        // given a permission its entries are left as they were, and the first
        // access that needs it lands here. An entry that has all it needs is
        // stale in another CPU's TLB, and the faulting instruction succeeds on
        // retry.
        auto needs = MmuFlags{};
        if (will_write) {
            needs |= MmuFlags::Writable;
        }
        if (will_execute) {
            needs |= MmuFlags::Executable;
        }
        if (!(needs & ~existing_mmuf)) {
            return {Status::Ok, 0};
        }
        return widen_present_locked(vmf, va, existing, mmuf, will_write);
    }

    // Only a paged VMO has pages to demand. map_physical() installs a physical
    // VMO's entire window when the mapping is created, because there is no
    // later moment at which a physical page could arrive -- so a fault on one
    // is not a missing page, it is an access that should not have happened.
    auto *paged = as<VmObjectPaged>(m_vmo.as_ptr_mut());
    if (!paged) {
        return {Status::NotSupported, 0};
    }

    // The object's lock, inside the address space lock the caller holds. The
    // page tree is guarded by this lock alone, and a fault used to walk and
    // insert into it without it -- so two address spaces faulting on one VMO,
    // or a fault racing a write() to it, wrote the tree concurrently. Held up
    // to the page table entry, so the page cannot be decommitted between
    // being found and being mapped.
    Guard guard{paged->lock()};

    // The same conversion map_paged() does, for the same reason: the cursor is
    // positioned in the VMO's byte space and |va| is an address.
    auto cursor = paged->make_cursor(m_vmo_ofs + (va - m_base), PAGE_SIZE);
    if (!cursor) {
        return {cursor.unwrap_err(), 0};
    }
    // The hardware retries the access through the entry made here, whose
    // accessed bit records it. A fault the kernel raised for a copy marks the
    // page as well: twice costs nothing.
    if (!(vmf->cause & VmFault::Cause::Software)) {
        cursor->disable_mark_accessed();
    }

    // One page per fault. vmf->num_pages says how many the caller would take,
    // and mapping ahead is worth having, but it needs the cursor to hand back
    // runs instead of single pages -- the batching map_paged()'s TODO asks
    // for. Asking for more than one before that exists would only mean more
    // single-page work under the same lock.
    auto page = cursor->require_page(will_write, 1, &vmf->page_request);
    if (!page) {
        // ShouldWait for a VMO with a page source: the page is asked for and
        // not there yet. It goes back up as it is, and page_fault_internal()
        // lets go of this lock and the address space's, waits on the request in
        // |vmf| and comes back. An anonymous VMO never answers it: "nothing
        // here" is a page allocated on the spot.
        return {page.unwrap_err(), 0};
    }

    // The whole 2 MiB around the page as one leaf, when the page is part of a
    // block and everything else allows one: the 511 faults after this one do
    // not happen, and the TLB holds one entry for all of them.
    if (auto const mapped = map_block_locked(*paged, ustl::round_down(va, LARGE_LEAF), mmuf)) {
        return *mapped;
    }

    // A page that is not the node's to write -- content on loan from an
    // ancestor, or the shared zero page -- is mapped without Write even in a
    // mapping that allows it. The mapping's own permission stays as it is; only
    // the entry lacks Write, so a write to the page faults again, asks for a page
    // of its own, and gets the copy (widen_present_locked()). Mapped with Write,
    // the first write would go through to every node that shares the page.
    auto page_mmuf = mmuf;
    if (!page->writable) {
        page_mmuf &= ~MmuFlags::Writable;
    }

    // ErrorIfExisting states the expectation rather than papering over its
    // failure: the query above established that the entry is absent, so
    // anything present by now was installed while this ran, which the address
    // space lock is supposed to prevent. The control is inert today
    // (PageTable::map_pages() ignores it and errors regardless), so this only
    // records the intent for when the arch layer starts reading it.
    usize nr_mapped = 0;
    if (auto const status = arch_aspace.map(
            va, frame_to_phys(page->page), 1, page_mmuf,
            ArchMapOptions::ErrorIfExisting | ArchMapOptions::UndoOnFailure, &nr_mapped);
        Status::Ok != status) {
        return {status, 0};
    }

    return {Status::Ok, nr_mapped};
}

/// Gives an entry that is present a permission an access needs and it lacks.
///
/// The only place a permission is ever added to an entry: protection narrows
/// and this widens, and it does so by mapping the page again, so that the frame
/// is named and whoever names it has shown the entry may have the permission.
/// For a write to a paged object that is `require_owned_page()`: the page the
/// writer gets is its own, a copy of the shared one or the shared one itself
/// when the writer was the last to see it, and mapped with Write. The frame
/// may be the one already mapped or another; either way the entry is replaced.
/// An access that needs only Execute keeps the entry's frame and whatever Write
/// it has, since it proves nothing about the page being the writer's.
///
/// Unmap and then map, under the address space's lock the fault holds, so no
/// other fault in this address space sees the gap. Another CPU running it and
/// touching the page in the gap faults, waits for that lock, and finds the entry
/// present. What such a CPU holds in its TLB of the old entry is the open
/// question of the design: a stale entry with less than the new one only faults
/// again, which is safe, but a fork leaves the old frame mapped read-only there
/// until it is flushed, and a reader on that CPU sees the page as it was.
auto VmMapping::widen_present_locked(VmFault *vmf, VirtAddr va, PhysAddr existing, MmuFlags mmuf,
                                     bool will_write) -> ustl::Pair<Status, usize> {
    auto pa = existing;
    auto new_mmuf = mmuf;
    if (auto *paged = as<VmObjectPaged>(m_vmo.as_ptr_mut())) {
        // Same lock, same order as the demand path: the object's, inside the
        // address space's.
        Guard guard{paged->lock()};
        if (will_write) {
            auto cursor = paged->make_cursor(m_vmo_ofs + (va - m_base), PAGE_SIZE);
            if (!cursor) {
                return {cursor.unwrap_err(), 0};
            }
            // As the demand path: the entry records a hardware access.
            if (!(vmf->cause & VmFault::Cause::Software)) {
                cursor->disable_mark_accessed();
            }
            auto page = cursor->require_owned_page(1, &vmf->page_request);
            if (!page) {
                return {page.unwrap_err(), 0};
            }
            pa = frame_to_phys(page->page);
        } else {
            // |existing| was read before this lock was taken, and a frame leaves
            // the object only under it, after every entry for it is unmapped;
            // Write is taken off entries the same way (a clone's
            // range_change_locked()). So the entry is read again now that the
            // lock is held. One that is gone, or names another frame, is not the
            // one this fault saw: the access retries, and finds what is there now.
            // Mapping |existing| again would map a frame the object no longer has.
            PhysAddr now{};
            MmuFlags now_mmuf{};
            if (Status::Ok != m_aspace->arch_aspace().query(va, &now, &now_mmuf) ||
                now != existing) {
                return {Status::Ok, 0};
            }
            // Nothing was shown about the page: it keeps the Write it has.
            new_mmuf = (mmuf & ~MmuFlags::Writable) | (now_mmuf & MmuFlags::Writable);
        }

        // Inside the object's lock, so the page cannot be taken from the object
        // between being chosen and being mapped.
        return remap_locked(va, pa, new_mmuf);
    }

    // A physical object has no copy-on-write: the frame is the mapping's, and
    // the region says what it may have.
    return remap_locked(va, pa, new_mmuf);
}

/// Everything a 2 MiB leaf needs, cheapest first:
///
///  - a user address space: only its page table sets a table aside for each
///    leaf and splits leaves when part of one changes; the kernel's never
///    does, so a leaf there could never be changed in part;
///  - the window inside this mapping, and one protection over all of it, the
///    one the caller has for the page it is mapping: a leaf has one set of
///    permissions;
///  - the window's entry empty in the page table: no leaf, no table (a split
///    leaves one behind for good);
///  - the 512 pages the object shows there one block (VmObjectPaged::
///    find_block_locked()): one frame and the 511 after it, 2 MiB aligned, all
///    held by one node. That says too whether the leaf may have Write.
///
/// The map can still end in small entries -- when no table can be set aside
/// for the leaf -- and then it maps all 512, which is as correct and only
/// costs more entries. Nothing here needs a TLB invalidation: the entries are
/// new.
auto VmMapping::map_block_locked(VmObjectPaged &vmo, VirtAddr const window, MmuFlags const mmuf)
    -> ustl::Option<ustl::Pair<Status, usize>> {
    if (!m_aspace->is_user()) {
        return ustl::NONE;
    }
    if (!ustl::is_aligned(window, LARGE_LEAF) || window < m_base || m_size < LARGE_LEAF ||
        window - m_base > m_size - LARGE_LEAF) {
        return ustl::NONE;
    }
    auto enumerator = m_protection_ranges.make_enumerator(window, LARGE_LEAF);
    auto const region = enumerator.next();
    if (!region || region.size != LARGE_LEAF || region.mmuf != mmuf) {
        return ustl::NONE;
    }

    auto &arch_aspace = m_aspace->arch_aspace();
    if (!arch_aspace.large_leaf_slot_empty(window)) {
        return ustl::NONE;
    }
    vmo.lock().assert_held();
    auto const block = vmo.find_block_locked(m_vmo_ofs + (window - m_base));
    if (!block) {
        return ustl::NONE;
    }

    auto leaf_mmuf = mmuf;
    if (!block->writable) {
        leaf_mmuf &= ~MmuFlags::Writable;
    }
    usize nr_mapped = 0;
    auto const status = arch_aspace.map(
        window, block->base, LARGE_LEAF / PAGE_SIZE, leaf_mmuf,
        ArchMapOptions::ErrorIfExisting | ArchMapOptions::UndoOnFailure, &nr_mapped);
    return ustl::some(ustl::Pair<Status, usize>{status, nr_mapped});
}

auto VmMapping::remap_locked(VirtAddr va, PhysAddr pa, MmuFlags mmuf) -> ustl::Pair<Status, usize> {
    auto &arch_aspace = m_aspace->arch_aspace();
    if (auto const status = arch_aspace.unmap(va, 1, UnmapOptions::None, nullptr);
        Status::Ok != status) {
        return {status, 0};
    }
    usize nr_mapped = 0;
    if (auto const status = arch_aspace.map(
            va, pa, 1, mmuf, ArchMapOptions::ErrorIfExisting | ArchMapOptions::UndoOnFailure,
            &nr_mapped);
        Status::Ok != status) {
        // The entry is gone and the next access faults and maps it afresh, which is
        // the safe way for this to fail.
        return {status, 0};
    }
    return {Status::Ok, nr_mapped};
}

INIT_CODE
auto init_vm_mapping_cache() -> void {
    if (Status::Ok != VmMappingCache::init("vm-mapping-cache", ObjectCache::Flags::Folio)) {
        panic("Failed to create object cache for VmMapping");
    }
    log::trace("VmMappingCache has been created");
}
// Heap, not PlatformEarly: this creates a MemCache, so it needs the object
// caches to be up.
GKTL_INIT_HOOK(VmMappingCacheInit, init_vm_mapping_cache, gktl::InitLevel::Heap);

} // namespace ours::mm
