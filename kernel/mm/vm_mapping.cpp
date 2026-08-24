#include <gktl/init_hook.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object-cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ustl/algorithms/search.hpp>
#include <ustl/iterator/function.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
using VmMappingCache = ObjectCache::Instance<VmMapping>;
using ProtectionRegionCache = ObjectCache::Instance<MappingProtectionRanges::Region>;

template <typename... Args>
FORCE_INLINE auto MappingProtectionRanges::alloc_region(Args &&...args) -> Region * {
    return ProtectionRegionCache::get(kGafKernel, args...);
}

FORCE_INLINE
auto MappingProtectionRanges::free_region(Region *region) -> void {
    ProtectionRegionCache::put(region);
}

FORCE_INLINE
auto MappingProtectionRanges::init(VirtAddr end, MmuFlags mmuf) -> Status {
    auto first_region = alloc_region(end, mmuf);
    if (!first_region) {
        return Status::OutOfMem;
    }

    regions_.insert(*first_region);
    return Status::Ok;
}

auto MappingProtectionRanges::make_enumerator(VirtAddr base, usize size) -> Enumerator {
    using namespace ustl::algorithms;
    auto first = lower_bound(regions_.begin(), regions_.end(), base,
                             [](auto const &x, auto y) { return x.end <= y; });

    auto last = upper_bound(first, regions_.end(), base + size,
                            [](auto x, auto const &y) { return x <= y.end; });

    if (last != regions_.end()) {
        ++last;
    }

    return Enumerator(base, first, last, base, base + size);
}

auto MappingProtectionRanges::update(VirtAddr base, usize size, MmuFlags mmuf, VirtAddr lower_limit,
                                     VirtAddr upper_limit) -> Status {
    using namespace ustl::algorithms;

    VirtAddr end = base + size;
    if (base >= end || base < lower_limit || end > upper_limit) {
        return Status::InvalidArguments;
    }

    // LM is the region containing base, while RM is the region containing
    // the last byte of the half-open range [base, end).
    auto lm = lower_bound(regions_.begin(), regions_.end(), base,
                          [](auto const &x, auto y) { return x.end <= y; });
    auto rm = lower_bound(lm, regions_.end(), end, [](auto const &x, auto y) { return x.end < y; });

    if (lm == regions_.end() || rm == regions_.end()) {
        return Status::InvalidArguments;
    }
    if (lm == rm && lm->mmuf == mmuf) {
        return Status::Ok;
    }

    // A boundary at base belongs to the region on its left. If base is
    // inside LM, create that boundary before changing any existing node.
    auto base_boundary = regions_.end();
    if (lm != regions_.begin()) {
        auto previous = lm;
        --previous;
        if (previous->end == base) {
            base_boundary = previous;
        }
    }

    Region *base_split = nullptr;
    if (base_boundary == regions_.end() && base != lower_limit && lm->mmuf != mmuf) {
        base_split = alloc_region(base, lm->mmuf);
        if (!base_split) {
            return Status::OutOfMem;
        }
    }

    // A separate boundary at end is needed only when RM cannot absorb
    // the updated range. Otherwise RM already has the desired flags.
    Region *end_split = nullptr;
    if (rm->end != end && rm->mmuf != mmuf) {
        end_split = alloc_region(end, rm->mmuf);
        if (!end_split) {
            if (base_split) {
                free_region(base_split);
            }
            return Status::OutOfMem;
        }
    }

    if (base_split) {
        base_boundary = regions_.insert(lm, *base_split);
    }

    auto end_boundary = rm;
    if (end_split) {
        end_boundary = regions_.insert(rm, *end_split);
    }

    auto target_first = lm;
    if (base_boundary != regions_.end()) {
        target_first = base_boundary;
        ++target_first;
    }

    // Keep the left boundary when it already has the new flags. Otherwise
    // target_first is the boundary that represents the updated range.
    auto keep_boundary = target_first;
    auto delete_first = target_first;
    if (base_boundary != regions_.end() && base_boundary->mmuf == mmuf) {
        keep_boundary = base_boundary;
    } else {
        ++delete_first;
    }

    VirtAddr final_end = end_boundary->end;
    auto delete_last = end_boundary;
    ++delete_last;
    if (delete_last != regions_.end() && delete_last->mmuf == mmuf) {
        final_end = delete_last->end;
        ++delete_last;
    }

    // All obsolete nodes are contiguous after keep_boundary. Delete them
    // together before changing keep_boundary->end, which is the tree key.
    if (delete_first != delete_last) {
        regions_.erase_and_dispose(delete_first, delete_last,
                                   [](Region *region) { Self::free_region(region); });
    }
    keep_boundary->end = final_end;
    keep_boundary->mmuf = mmuf;

    return Status::Ok;
}

/// This helper class was used to batch mapping requests.
template <usize MaxNumPages>
class MappingCoalescer {
  public:
    MappingCoalescer(VmMapping *mapping, VirtAddr base, MmuFlags mmuf, MapControl ctrl)
        : mapping_(mapping)
        , va_(base)
        , mmuf_(mmuf)
        , nr_pages_(0)
        , map_ctrl_(ctrl)
        , total_mapped_(0) {}

    ~MappingCoalescer() {
        DEBUG_ASSERT(nr_pages_ == 0);
    }

    auto append(PhysAddr phys) -> Status;
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
    auto status = mapping_->aspace()->arch_aspace().map_bulk(va_, pa_, nr_pages_, mmuf_,
                                                             MapControl::TryLargePage, &nr_mapped);
    if (Status::Ok != status) {
        log::error("Failed to map {} pages at {}", nr_pages_, va_);
    }

    va_ += nr_pages_ * PAGE_SIZE;
    nr_pages_ = 0;
    total_mapped_ += nr_mapped;
    return Status::Ok;
}

VmMapping::VmMapping(VmArea *parent, VirtAddr base, usize size, VmaFlags vmaf,
                     ustl::Arc<VmObject> vmo, usize vmo_off, const char *name)
    : Base(kMappingMagic, base, size, vmaf | VmaFlags::Mapping, parent,
           parent->aspace().as_ptr_mut(), name)
    , m_vmo_ofs(vmo_off)
    , m_vmo(ustl::move(vmo))
    , m_protection_ranges() {}

auto VmMapping::create(VmArea *parent, VirtAddr base, usize size, VmaFlags vmaf,
                       ustl::Arc<VmObject> vmo, usize vmo_off, MmuFlags mmuf, char const *name,
                       ustl::Arc<Self> *out) -> Status {
    if (!vmo) {
        return Status::InvalidArguments;
    }

    auto mapping =
        VmMappingCache::get(kGafKernel, parent, base, size, vmaf, ustl::move(vmo), vmo_off, name);
    if (!mapping) {
        return Status::OutOfMem;
    }
    auto status = mapping->m_protection_ranges.init(base + size, mmuf);
    if (Status::Ok != status) {
        VmMappingCache::put(mapping);
        return status;
    }
    mapping->activate();

    *out = ustl::Arc<Self>::adopt(mapping);
    return Status::Ok;
}

auto VmMapping::activate() -> Status {
    m_canary.verify(kMappingMagic);

    Base::activate();

    // Note when a mapping was added into a VMO's mapping list, the inside counter
    // has not been increased implicitly. It is required to pay attention to manually
    // install and uninstall a mapping, avoiding lifecycle problem.
    m_vmo->add_mapping(*this);

    return Status::Ok;
}

auto VmMapping::destroy() -> Status {
    m_canary.verify(kMappingMagic);

    auto status = m_aspace->arch_aspace().unmap(m_base, m_size >> PAGE_SHIFT, {}, 0);
    if (Status::Ok != status) {
        log::trace("Failed to destroy Mapping with unmapping {} pages at {}", m_size >> PAGE_SHIFT,
                   m_base);
        return status;
    }

    if (m_vmo) {
        m_vmo->remove_mapping(*this);
    }
    Base::destroy();
    return Status::Ok;
}

FORCE_INLINE
auto VmMapping::map_paged(VmObjectPaged *vmo, VirtAddr base, usize size, bool commit,
                          MapControl control) -> Status {
    m_canary.verify(kMappingMagic);

    DEBUG_ASSERT(ustl::mm::is_aligned(base, PAGE_SIZE));
    DEBUG_ASSERT(ustl::mm::is_aligned(size, PAGE_SIZE));

    // FIXME: `commit` parameter is ignored. The caller's intent to defer
    // commit (commit=false) is silently lost — pages are always committed
    // immediately via the coalescer.
    (void)commit;

    constexpr static auto const kMaxBatchPages = 32;

    auto cursor = vmo->make_cursor(base, size);
    if (!cursor) {
        return cursor.unwrap_err();
    }

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto region = enumerator.next()) {
        auto [base, size, mmuf] = *region;
        MappingCoalescer<kMaxBatchPages> coalescer(this, base, mmuf, control);

        // TODO: Request pages in larger batches (e.g. folios) instead of
        // page-by-page.  Currently require_owned_page(1) is called per
        // page, which prevents the VMO from returning compound pages and
        // also precludes the arch layer from using large-page mappings.
        MultiPageRequest page_request;
        for (auto i = 0; i < size; i += PAGE_SIZE) {
            auto result = cursor->require_owned_page(1, &page_request);
            if (!result) {
                return result.unwrap_err();
            }

            coalescer.append(frame_to_phys(*result));
        }
        // Commit those uncovered units in for loop above.
        coalescer.commit();
    }

    return Status::Ok;
}

FORCE_INLINE
auto VmMapping::map_physical(VmObjectPhysical *vmo, VirtAddr base, usize size, MapControl control)
    -> Status {
    m_canary.verify(kMappingMagic);

    DEBUG_ASSERT(vmo);
    PhysAddr phys_base;
    auto status = vmo->lookup_range(base - m_base, size, &phys_base);
    if (Status::Ok != status) {
        return status;
    }

    constexpr static auto const kMaxBatchPages = 32;

    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto region = enumerator.next()) {
        auto [base, size, mmuf] = *region;
        MappingCoalescer<kMaxBatchPages> coalescer(this, base, mmuf, control);
        for (auto offset = 0; offset < size; offset += PAGE_SIZE) {
            coalescer.append(phys_base + offset);
        }

        coalescer.commit();
    }

    return Status::Ok;
}

/// Do check arguments and dispatch the request to correct sub-routine for different VmObjects.
auto VmMapping::map(VirtAddr offset, usize size, bool commit, MapControl control) -> Status {
    m_canary.verify(kMappingMagic);

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
    m_canary.verify(kMappingMagic);

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
    auto status = m_protection_ranges.update(base, size, mmuf, m_base, m_base + m_size);
    if (Status::Ok != status) {
        return status;
    }

    // TODO: update() already traverses the affected tree nodes internally.
    // The subsequent make_enumerator() repeats the same lower_bound/upper_bound
    // O(log n) lookup and re-iterates over the same range.  Consider having
    // update() return an Enumerator directly so the caller can consume the
    // modified regions without a second traversal.
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto region = enumerator.next()) {
        auto &[base, size, mmuf] = *region;

        if (!(mmuf & MmuFlags::PermMask)) {
            // Unmap those regions which has no any permission.
            arch_aspace.unmap(base, size >> PAGE_SHIFT, UnmapControl::None, 0);
            continue;
        }

        arch_aspace.protect(base, size >> PAGE_SHIFT, mmuf);
    }

    return Status::Ok;
}

auto VmMapping::unmap(usize offset, usize size, UnmapControl control) -> Status {
    m_canary.verify(kMappingMagic);

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

    // FIXME: After unmapping page-table entries below, m_protection_ranges
    // is NOT updated to reflect the unmapped state.  A subsequent protect()
    // on the unmapped range would try to modify page-table entries that no
    // longer exist.  Consider calling m_protection_ranges.update() here to
    // mark the range with empty permissions (or track the unmapped state
    // explicitly).
    auto &arch_aspace = m_aspace->arch_aspace();
    auto enumerator = m_protection_ranges.make_enumerator(base, size);
    while (auto region = enumerator.next()) {
        auto [base, size, mmuf] = *region;
        auto status = arch_aspace.unmap(base, size >> PAGE_SHIFT, control, 0);
        if (Status::Ok != status) {
            log::error("Failed to unmap range[{:X}, {:X})", base, size);
            return status;
        }
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
    m_canary.verify(kMappingMagic);
    return {Status::NotSupported, 0};
}

INIT_CODE
auto init_vm_mapping_cache() -> void {
    s_vm_mapping_cache = ObjectCache::create<VmMapping>("vm-mapping-cache", OcFlags::Folio);
    if (!s_vm_mapping_cache) {
        panic("Failed to create object cache for VmMapping");
    }
    log::trace("VmMappingCache has been created");

    s_vm_mapping_region_cache = ObjectCache::create<MappingProtectionRanges::Region>(
        "vm-mapping-region-cache", OcFlags::Folio);
    // FIXME: The old code checked s_vm_mapping_cache (wrong pointer) and the
    // panic message still says "VmArea".  Fixed the guard but keeping this
    // note until tests confirm the region cache is alive.
    if (!s_vm_mapping_region_cache) {
        panic("Failed to create object cache for MappingProtectionRanges::Region");
    }
    log::trace("MappingRegionCache has been created");
}
GKTL_INIT_HOOK(VmMappingCacheInit, init_vm_mapping_cache, gktl::InitLevel::PlatformEarly);

} // namespace ours::mm
