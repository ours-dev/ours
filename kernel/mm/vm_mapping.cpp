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

/// This helper class was used to batch mapping requests.
template <usize MaxNumPages>
class MappingCoalescer {
  public:
    MappingCoalescer(VmMapping *mapping, VirtAddr base, MmuFlags mmuf, MapOptions ctrl)
        // Initializer order follows the member declaration order below.
        : mapping_(mapping)
        , va_(base)
        , nr_pages_(0)
        , total_mapped_(0)
        , mmuf_(mmuf)
        , map_ctrl_(ctrl) {}

    ~MappingCoalescer() {
        DEBUG_ASSERT(nr_pages_ == 0);
    }

    /// Both are [[nodiscard]]: a dropped status here is invisible at run time
    /// -- the batch is silently not mapped, and the mapping object is handed
    /// back looking successful. That is exactly how it used to fail.
    [[nodiscard]]
    auto append(PhysAddr phys) -> Status;

    [[nodiscard]]
    auto commit() -> Status;

  private:
    VmMapping *mapping_;
    VirtAddr va_;
    PhysAddr pa_[MaxNumPages];
    usize nr_pages_;
    usize total_mapped_;
    MmuFlags mmuf_;
    MapOptions map_ctrl_;
};

template <usize MaxNumPages>
FORCE_INLINE
auto MappingCoalescer<MaxNumPages>::append(PhysAddr phys) -> Status {
    pa_[nr_pages_++] = phys;
    if (nr_pages_ == MaxNumPages) {
        return commit();
    }
    return Status::Ok;
}

template <usize MaxNumPages>
FORCE_INLINE
auto MappingCoalescer<MaxNumPages>::commit() -> Status {
    if (!nr_pages_) {
        return Status::Ok;
    }

    usize nr_mapped = 0;
    auto const status = mapping_->aspace()->arch_aspace().map_bulk(
        va_, pa_, nr_pages_, mmuf_, MapOptions::TryLargePage, &nr_mapped);
    if (Status::Ok != status) {
        log::error("Failed to map {} pages at {}", nr_pages_, va_);
    }

    // Drained either way, and before the early return: the destructor asserts
    // the batch is empty, so a caller propagating the failure still has to be
    // able to let this object go. `total_mapped_` takes whatever the arch
    // layer managed, which on a partial failure is less than the batch.
    va_ += nr_pages_ * PAGE_SIZE;
    nr_pages_ = 0;
    total_mapped_ += nr_mapped;

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

auto VmMapping::create(VmArea *parent, VirtAddr base, usize size, VmaFlags vmaf,
                       ustl::Arc<VmObject> vmo, usize vmo_off, MmuFlags mmuf, char const *name,
                       ustl::Arc<Self> *out) -> Status {
    if (!vmo) {
        return Status::InvArgs;
    }

    auto mapping =
        VmMappingCache::get(GAF_KERNEL, parent, base, size, vmaf, ustl::move(vmo), vmo_off, name);
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
    mapping->activate();

    *out = ustl::Arc<Self>::adopt(mapping);
    return Status::Ok;
}

auto VmMapping::activate() -> Status {
    m_canary.verify<Self>();

    Base::activate();

    // Note when a mapping was added into a VMO's mapping list, the inside counter
    // has not been increased implicitly. It is required to pay attention to manually
    // install and uninstall a mapping, avoiding lifecycle problem.
    //
    // Under the object's lock, which the list belongs to; see
    // VmObject::add_mapping(). Zircon's VmMapping::Activate() takes
    // object_->lock() around ActivateLocked() the same way.
    Guard guard{m_vmo->lock()};
    m_vmo->add_mapping(*this);

    return Status::Ok;
}

auto VmMapping::destroy() -> Status {
    m_canary.verify<Self>();

    // Once only. Everything below undoes something activate() did exactly
    // once -- the VMO's list, the parent's count and set -- so a second pass
    // would unlink a node that is no longer linked and count the parent down
    // twice. Zircon's DestroyLocked() refuses a mapping that is no longer
    // alive in the same way.
    if (!is_active()) {
        return Status::BadState;
    }

    // The address space's last-fault cache may be this mapping, and it holds
    // a reference: left there, it would keep the mapping -- and through it the
    // VMO and the address space itself -- alive after the mapping is gone,
    // and hand the next fault in this range a mapping that no longer covers
    // anything. Cleared first, as Zircon's DestroyLockedObject() does, because
    // clearing it is right whatever happens below.
    if (m_aspace->m_last_fault.as_ptr() == this) {
        m_aspace->m_last_fault = {};
    }

    // Take the mapping out of the page tables first, but do not let a failure
    // here abandon the bookkeeping below. This used to return early, which left
    // the mapping linked into its VMO's mapping list and its parent's subvma
    // set; the last Arc then dropped, ~VmMapping ran on a still-linked node,
    // and ustl's intrusive hook trapped on it. A failed unmap is worth
    // reporting, but a destroyed object left on two live lists is a corrupt
    // list, which is strictly worse.
    auto const status = m_aspace->arch_aspace().unmap(m_base, m_size >> PAGE_SHIFT, {}, 0);
    if (Status::Ok != status) {
        log::trace("Failed to destroy Mapping with unmapping {} pages at {}", m_size >> PAGE_SHIFT,
                   m_base);
    }

    // And let go of the VMO. The mapping object can outlive its destruction
    // -- anyone holding an Arc to it keeps it -- and while it kept m_vmo, it
    // kept every page of the object too: a destroyed mapping of a program's
    // image pinned the image. A mapping that maps nothing has no business
    // with a VMO. Zircon resets object_ at the same point.
    if (m_vmo) {
        {
            Guard guard{m_vmo->lock()};
            m_vmo->remove_mapping(*this);
        }
        m_vmo = {};
    }
    if (m_parent) {
        m_parent->m_num_mappings -= 1;
    }
    Base::destroy();

    // Reported rather than swallowed: the caller learns the page tables may
    // still hold something, while the object itself is safely unregistered.
    return status;
}

FORCE_INLINE
auto VmMapping::map_paged(VmObjectPaged *vmo, VirtAddr base, usize size, bool commit,
                          MapOptions options) -> Status {
    m_canary.verify<Self>();

    DEBUG_ASSERT(ustl::mm::is_aligned(base, PAGE_SIZE));
    DEBUG_ASSERT(ustl::mm::is_aligned(size, PAGE_SIZE));

    // FIXME: `commit` parameter is ignored. The caller's intent to defer
    // commit (commit=false) is silently lost — pages are always committed
    // immediately via the coalescer.
    (void)commit;

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

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    // P0963R3: decomposing in the condition binds the region's fields straight
    // through, so the loop no longer opens by dereferencing an Option just to
    // give them names.
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf, options);

        // TODO: Request pages in larger batches (e.g. folios) instead of
        // page-by-page.  Currently require_owned_page(1) is called per
        // page, which prevents the VMO from returning compound pages and
        // also precludes the arch layer from using large-page mappings.
        MultiPageRequest page_request;
        for (usize i = 0; i < region_size; i += PAGE_SIZE) {
            auto result = cursor->require_owned_page(1, &page_request);
            if (!result) {
                return result.unwrap_err();
            }

            // An owned page is asked for, so it is always writable; a mapping
            // that takes borrowed pages will have to ask for reads instead and
            // map what comes back by |writable|.
            DEBUG_ASSERT(result->writable);
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
auto VmMapping::map_physical(VmObjectPhysical *vmo, VirtAddr base, usize size, MapOptions options)
    -> Status {
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
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf, options);
        for (usize offset = 0; offset < region_size; offset += PAGE_SIZE) {
            if (auto const appended = coalescer.append(phys_base + offset);
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
auto VmMapping::map(VirtAddr base, usize size, bool commit, MapOptions options) -> Status {
    m_canary.verify<Self>();

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

    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvArgs;
    }

    base = ustl::mm::align_down(base, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvArgs;
    }

    if (auto paged = as<VmObjectPaged>(m_vmo.as_ptr_mut())) {
        return map_paged(paged, base, size, commit, options);
    } else if (auto physical = as<VmObjectPhysical>(m_vmo.as_ptr_mut())) {
        return map_physical(physical, base, size, options);
    }

    return Status::Ok;
}

auto VmMapping::protect(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    m_canary.verify<Self>();

    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvArgs;
    }

    Guard guard{get_lock()};
    base = ustl::mm::align_down(base, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvArgs;
    }

    if (!validate_mmuflags(mmuf)) {
        return Status::InvArgs;
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
    //
    // Zircon has the mapping's protection ranges under the object's lock as
    // well (EnumerateProtectionRangesLockedObject()).
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
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [valid, region_base, region_size, region_mmuf] = enumerator.next()) {
        if (!(region_mmuf & MmuFlags::PermMask)) {
            // Unmap those regions which has no any permission.
            (void)arch_aspace.unmap(region_base, region_size >> PAGE_SHIFT, UnmapOptions::None, 0);
            continue;
        }

        arch_aspace.protect(region_base, region_size >> PAGE_SHIFT, region_mmuf);
    }

    return Status::Ok;
}

auto VmMapping::remove_write_locked_object(u64 const vmo_offset, u64 const len) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(m_vmo);
    m_vmo->lock().assert_held();

    // The part of the range this mapping shows, in the object's bytes and then
    // as addresses. |len| may reach the end of the address space, so the end is
    // clamped rather than added.
    u64 end = vmo_offset + len;
    if (len > ~u64{0} - vmo_offset) {
        end = ~u64{0};
    }
    u64 const first = std::max<u64>(vmo_offset, m_vmo_ofs);
    u64 const last = std::min<u64>(end, m_vmo_ofs + m_size);
    if (first >= last) {
        return;
    }
    VirtAddr const base = m_base + (first - m_vmo_ofs);
    usize const size = last - first;

    // Only what can be written is worth touching: a region without Write has
    // page table entries without it, since entries never have more than their
    // region. What is kept is Read and Execute, whatever the region has of them.
    auto &arch_aspace = m_aspace->arch_aspace();
    constexpr auto KEEP = MmuFlags::PermMask & ~MmuFlags::Writable;
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [valid, region_base, region_size, region_mmuf] = enumerator.next()) {
        if (!(region_mmuf & MmuFlags::Writable)) {
            continue;
        }
        auto const status = arch_aspace.restrict(region_base, region_size >> PAGE_SHIFT, KEEP);
        ASSERT(Status::Ok == status,
               "could not take Write off a mapping of pages about to be shared");
    }
}

auto VmMapping::unmap(VirtAddr base, usize size, UnmapOptions options) -> Status {
    m_canary.verify<Self>();

    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvArgs;
    }

    Guard guard{get_lock()};
    base = ustl::mm::align_down(base, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvArgs;
    }

    return unmap_locked(base, size, options);
}

auto VmMapping::unmap_locked(VirtAddr base, usize size, UnmapOptions options) -> Status {
    // The object's lock, for what protect_locked() takes it for: it writes
    // m_protection_ranges, which an operation reaching this mapping from the
    // object's side reads without the address space's lock. See there.
    DEBUG_ASSERT(m_vmo);
    Guard guard{m_vmo->lock()};

    auto &arch_aspace = m_aspace->arch_aspace();
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        auto status = arch_aspace.unmap(region_base, region_size >> PAGE_SHIFT, options, 0);
        if (Status::Ok != status) {
            log::error("Failed to unmap range[{:X}, {:X})", region_base, region_size);
            return status;
        }
    }

    // Mark the range as permission-less, the same representation of "unmapped"
    // that protect() relies on when it skips zero-permission regions; without
    // this, a later protect() would try to modify page-table entries that no
    // longer exist.
    auto status = m_protection_ranges.update(base, size, MmuFlags{});
    if (Status::Ok != status) {
        return status;
    }

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

    VirtAddr const va = ustl::mm::align_down(vmf->va, PAGE_SIZE);
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
    DEBUG_ASSERT(region.valid, "the protection regions do not tile this mapping");
    MmuFlags const mmuf = region.mmuf;

    // A region with no permission at all is how unmap() and protect() spell
    // "nothing is mapped here any more". Faulting a page back in would quietly
    // undo them, so this is a refusal, not a miss.
    if (!(mmuf & MmuFlags::PermMask)) {
        return {Status::AccessDenied, 0};
    }

    bool const will_write = !!(vmf->cause & VmfCause::Write);
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
    // spurious, and only this region says which case it is.
    //
    // Zircon's VmMapping::PageFaultLockedObject() adds
    // ARCH_MMU_FLAG_PERM_EXECUTE to the flags it requires for
    // VMM_PF_FLAG_INSTRUCTION, ahead of any lookup.
    bool const will_execute = !!(vmf->cause & VmfCause::Instruction);
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
    PhysAddr existing = 0;
    MmuFlags existing_mmuf{};
    if (Status::Ok == arch_aspace.query(va, &existing, &existing_mmuf)) {
        // An entry left behind by a protect() that widened the permissions
        // without reaching it. protect() is the only entry point that can
        // rewrite flags in place -- map() cannot, and unmapping first would
        // open a window in which the page is missing for everyone else.
        if ((existing_mmuf & MmuFlags::PermMask) != (mmuf & MmuFlags::PermMask)) {
            if (auto const status = arch_aspace.protect(va, 1, mmuf); Status::Ok != status) {
                return {status, 0};
            }
        }
        return {Status::Ok, 0};
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
    // being found and being mapped. Zircon's VmMapping::PageFault() takes
    // `Guard<CriticalMutex>{paged->lock()}` at the same point.
    Guard guard{paged->lock()};

    // The same conversion map_paged() does, for the same reason: the cursor is
    // positioned in the VMO's byte space and |va| is an address.
    auto cursor = paged->make_cursor(m_vmo_ofs + (va - m_base), PAGE_SIZE);
    if (!cursor) {
        return {cursor.unwrap_err(), 0};
    }

    // One page per fault. vmf->num_pages says how many the caller would take,
    // and mapping ahead is worth having, but it needs the cursor to hand back
    // runs instead of single pages -- the batching map_paged()'s TODO asks
    // for. Asking for more than one before that exists would only mean more
    // single-page work under the same lock.
    auto page = cursor->require_page(will_write, 1, &vmf->page_request);
    if (!page) {
        // For an anonymous VMO this is never ShouldWait: there is nobody to
        // wait for, so "nothing here" is answered by allocating on the spot.
        // page_fault_internal()'s retry loop is therefore never entered today.
        return {page.unwrap_err(), 0};
    }

    // |page->writable| is not consulted, and has to be the day it can be false.
    // A read of a node that hangs under another is answered with a page on
    // loan, or the shared zero page, and that must be mapped without Write even
    // in a mapping that allows it, or the first write would go through to the
    // lender's page instead of faulting. No such node is behind a mapping yet,
    // so this asserts rather than handles, and stops the day one is.
    DEBUG_ASSERT(page->writable, "the fault path does not yet map a page on loan read-only");

    //
    // ErrorIfExisting states the expectation rather than papering over its
    // failure: the query above established that the entry is absent, so
    // anything present by now was installed while this ran, which the address
    // space lock is supposed to prevent. The control is inert today
    // (PageTable::map_pages() ignores it and errors regardless), so this only
    // records the intent for when the arch layer starts reading it.
    usize nr_mapped = 0;
    if (auto const status = arch_aspace.map(va, frame_to_phys(page->page), 1, mmuf,
                                            MapOptions::ErrorIfExisting, &nr_mapped);
        Status::Ok != status) {
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
