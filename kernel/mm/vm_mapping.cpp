#include <gktl/init_hook.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ustl/iterator/function.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
using VmMappingCache = ObjectCache::Instance<VmMapping>;

/// This helper class was used to batch mapping requests.
template <usize MaxNumPages>
class MappingCoalescer {
  public:
    MappingCoalescer(VmMapping *mapping, VirtAddr base, MmuFlags mmuf, MapControl ctrl)
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
    MapControl map_ctrl_;
};

template <usize MaxNumPages>
FORCE_INLINE auto MappingCoalescer<MaxNumPages>::append(PhysAddr phys) -> Status {
    pa_[nr_pages_++] = phys;
    if (nr_pages_ == MaxNumPages) {
        return commit();
    }
    return Status::Ok;
}

template <usize MaxNumPages>
FORCE_INLINE auto MappingCoalescer<MaxNumPages>::commit() -> Status {
    if (!nr_pages_) {
        return Status::Ok;
    }

    usize nr_mapped = 0;
    auto const status = mapping_->aspace()->arch_aspace().map_bulk(
        va_, pa_, nr_pages_, mmuf_, MapControl::TryLargePage, &nr_mapped);
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
    : Base(MAPPING_MAGIC, base, size, vmaf | VmaFlags::Mapping, parent,
           parent->aspace().as_ptr_mut(), name)
    // Initializer order follows the member declaration order in the header.
    , m_vmo(ustl::move(vmo))
    , m_vmo_ofs(vmo_off)
    , m_protection_ranges() {}

auto VmMapping::create(VmArea *parent, VirtAddr base, usize size, VmaFlags vmaf,
                       ustl::Arc<VmObject> vmo, usize vmo_off, MmuFlags mmuf, char const *name,
                       ustl::Arc<Self> *out) -> Status {
    if (!vmo) {
        return Status::InvalidArguments;
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
    mapping->activate();

    *out = ustl::Arc<Self>::adopt(mapping);
    return Status::Ok;
}

auto VmMapping::activate() -> Status {
    m_canary.verify(MAPPING_MAGIC);

    Base::activate();

    // Note when a mapping was added into a VMO's mapping list, the inside counter
    // has not been increased implicitly. It is required to pay attention to manually
    // install and uninstall a mapping, avoiding lifecycle problem.
    m_vmo->add_mapping(*this);

    return Status::Ok;
}

auto VmMapping::destroy() -> Status {
    m_canary.verify(MAPPING_MAGIC);

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

    if (m_vmo) {
        m_vmo->remove_mapping(*this);
    }
    if (m_parent) {
        m_parent->m_num_mappings -= 1;
    }
    Base::destroy();

    // Reported rather than swallowed: the caller learns the page tables may
    // still hold something, while the object itself is safely unregistered.
    return status;
}

FORCE_INLINE auto VmMapping::map_paged(VmObjectPaged *vmo, VirtAddr base, usize size, bool commit,
                                       MapControl control) -> Status {
    m_canary.verify(MAPPING_MAGIC);

    DEBUG_ASSERT(ustl::mm::is_aligned(base, PAGE_SIZE));
    DEBUG_ASSERT(ustl::mm::is_aligned(size, PAGE_SIZE));

    // FIXME: `commit` parameter is ignored. The caller's intent to defer
    // commit (commit=false) is silently lost — pages are always committed
    // immediately via the coalescer.
    (void)commit;

    constexpr static auto MAX_BATCH_PAGES = 32;

    auto cursor = vmo->make_cursor(base, size);
    if (!cursor) {
        return cursor.unwrap_err();
    }

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    // P0963R3: decomposing in the condition binds the region's fields straight
    // through, so the loop no longer opens by dereferencing an Option just to
    // give them names.
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf, control);

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

            if (auto const status = coalescer.append(frame_to_phys(*result));
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

FORCE_INLINE auto VmMapping::map_physical(VmObjectPhysical *vmo, VirtAddr base, usize size,
                                          MapControl control) -> Status {
    m_canary.verify(MAPPING_MAGIC);

    DEBUG_ASSERT(vmo);
    PhysAddr phys_base;
    auto status = vmo->lookup_range(base - m_base, size, &phys_base);
    if (Status::Ok != status) {
        return status;
    }

    constexpr static auto MAX_BATCH_PAGES = 32;

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        MappingCoalescer<MAX_BATCH_PAGES> coalescer(this, region_base, mmuf, control);
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
auto VmMapping::map(VirtAddr offset, usize size, bool commit, MapControl control) -> Status {
    m_canary.verify(MAPPING_MAGIC);

    // FIXME: map() does not acquire m_aspace->get_lock(), unlike protect()
    // and unmap() which both take the lock via Guard{get_lock()}.  This is
    // a potential data race if map() runs concurrently with
    // protect()/unmap() on the same mapping — page-table entries and
    // m_protection_ranges can be mutated from multiple threads.

    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvalidArguments;
    }

    VirtAddr base = ustl::mm::align_down(m_base + offset, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvalidArguments;
    }

    if (auto paged = as<VmObjectPaged>(m_vmo.as_ptr_mut())) {
        return map_paged(paged, base, size, commit, control);
    } else if (auto physical = as<VmObjectPhysical>(m_vmo.as_ptr_mut())) {
        return map_physical(physical, base, size, control);
    }

    return Status::Ok;
}

auto VmMapping::protect(usize offset, usize size, MmuFlags mmuf) -> Status {
    m_canary.verify(MAPPING_MAGIC);

    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvalidArguments;
    }

    Guard guard{get_lock()};
    VirtAddr base = ustl::mm::align_down(m_base + offset, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvalidArguments;
    }

    if (!validate_mmuflags(mmuf)) {
        return Status::InvalidArguments;
    }

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
            (void)arch_aspace.unmap(region_base, region_size >> PAGE_SHIFT, UnmapControl::None, 0);
            continue;
        }

        arch_aspace.protect(region_base, region_size >> PAGE_SHIFT, region_mmuf);
    }

    return Status::Ok;
}

auto VmMapping::unmap(usize offset, usize size, UnmapControl control) -> Status {
    m_canary.verify(MAPPING_MAGIC);

    if (!is_active()) {
        return Status::BadState;
    }

    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return Status::InvalidArguments;
    }

    Guard guard{get_lock()};
    VirtAddr base = ustl::mm::align_down(m_base + offset, PAGE_SIZE);
    if (!check_range(base, size)) {
        return Status::InvalidArguments;
    }

    return unmap_locked(base, size, control);
}

auto VmMapping::unmap_locked(VirtAddr base, usize size, UnmapControl control) -> Status {
    auto &arch_aspace = m_aspace->arch_aspace();
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto [valid, region_base, region_size, mmuf] = enumerator.next()) {
        auto status = arch_aspace.unmap(region_base, region_size >> PAGE_SHIFT, control, 0);
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

// FIXME: This is an incomplete stub — the function body has no return
// statement, which is undefined behavior for a function returning
// ustl::Pair<Status, usize>.  The fault path currently falls through to
// VmObject/VmObjectPaged handling; decide whether VmMapping should
// implement its own fault logic (e.g. CoW resolution) or delegate entirely
// to the VMO.
auto VmMapping::fault(VmFault *vmf) -> ustl::Pair<Status, usize> {
    m_canary.verify(MAPPING_MAGIC);
    return {Status::NotSupported, 0};
}

INIT_CODE auto init_vm_mapping_cache() -> void {
    if (Status::Ok != VmMappingCache::init("vm-mapping-cache", ObjectCache::Flags::Folio)) {
        panic("Failed to create object cache for VmMapping");
    }
    log::trace("VmMappingCache has been created");
}
// Heap, not PlatformEarly: this creates a MemCache, so it needs the object
// caches to be up.
GKTL_INIT_HOOK(VmMappingCacheInit, init_vm_mapping_cache, gktl::InitLevel::Heap);

} // namespace ours::mm
