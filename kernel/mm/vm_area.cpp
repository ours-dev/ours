#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>

namespace ours::mm {
using VmaCache =
    ObjectCache::Instance<VmArea,
                          ObjectCache::Option::AddInitHook<"VmaCache", ObjectCache::Flags::Folio,
                                                           gktl::InitLevel::PlatformEarly>>;

VmArea::VmArea(VirtAddr base, usize size, VmaFlags vmaf, Self *parent, VmAspace *aspace,
               char const *name)
    : Base(AREA_MAGIC, base, size, vmaf, parent, aspace, name), m_num_mappings(), m_subvmas() {}

auto VmArea::create_common(VirtAddr base, usize size, VmaFlags vmaf, VmArea *parent,
                           VmAspace *aspace, char const *name, ustl::Arc<Self> *out) -> Status {
    DEBUG_ASSERT(aspace, "Given a invalid aspace");
    DEBUG_ASSERT(out, "`out` can not be null");

    auto self = VmaCache::get(GAF_KERNEL, base, size, vmaf, parent, aspace, name);
    if (!self) {
        return Status::OutOfMem;
    }
    if (!parent) {
        // A root VMA has no parent so just mark it active.
        self->mark_active();
    } else {
        self->activate();
    }

    *out = ustl::Arc<VmArea>::adopt(self);
    return Status::Ok;
}

auto VmArea::alloc_spot(usize size, usize align, VirtAddr lower_limit, VirtAddr upper_limit)
    -> ktl::Result<VirtAddr> {
    m_canary.verify(AREA_MAGIC);
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }
    return m_subvmas.find_spot(size, align, lower_limit, upper_limit);
}

auto VmArea::create_subvma(usize vma_ofs, usize vma_size, VmaFlags vmaf, char const *name,
                           VmMapOption option, ustl::Arc<VmArea> *out) -> Status {
    m_canary.verify(AREA_MAGIC);

    auto const result = create_subaom_preamble(vma_ofs, vma_size, m_vmaf, option);
    if (result.is_err()) {
        return result.unwrap_err();
    }

    auto const [base, size] = *result;

    ustl::Arc<VmAreaOrMapping> aom;
    auto const status = Builder(&m_aspace, m_vmaf, size)
                            .set_parent(this)
                            .set_base(base)
                            .set_vmo_offset(vma_ofs)
                            .set_option(option)
                            .set_name(name)
                            .build(&aom);

    if (Status::Ok != status) {
        return status;
    }
    *out = ustl::downcast<VmArea>(ustl::move(aom));

    return Status::Ok;
}

auto VmArea::create_mapping(usize vma_ofs, usize vma_size, usize vmo_ofs, MmuFlags mmuf,
                            ustl::Arc<VmObject> vmo, char const *name, VmMapOption option,
                            ustl::Arc<VmMapping> *out) -> Status {
    m_canary.verify(AREA_MAGIC);

    auto const result = create_subaom_preamble(vma_ofs, vma_size, m_vmaf, option);
    if (result.is_err()) {
        return result.unwrap_err();
    }

    auto const [base, size] = *result;

    ustl::Arc<VmAreaOrMapping> out_aom;
    auto const status = Builder(&m_aspace, m_vmaf, size)
                            .set_parent(this)
                            .set_base(base)
                            .set_vmo(ustl::move(vmo))
                            .set_vmo_offset(vma_ofs)
                            .set_name(name)
                            .set_mmuf(mmuf)
                            .set_option(option)
                            .build(&out_aom);

    if (Status::Ok != status) {
        return status;
    }
    *out = ustl::downcast<VmMapping>(ustl::move(out_aom));
    m_num_mappings += 1;

    return Status::Ok;
}

auto VmArea::create_subaom_preamble(usize offset, usize size, VmaFlags vmaf, VmMapOption option)
    -> ktl::Result<ustl::Pair<VirtAddr, usize>> {
    if (!is_active()) {
        // Operate an illegal VMA.
        return ktl::err(Status::BadState);
    }

    // Check if the given MMU flags have the same or lower permissions than
    // those allowed by this VMA.
    if (!validate_vmaflags(vmaf)) {
        // Unallowed permissions.
        return ktl::err(Status::InvalidArguments);
    }

    auto [base_aligned, size_aligned] = resolve_page_range(m_base + offset, size);

    auto const is_fixed_noreplace = (!!(option & VmMapOption::FixedNoReplace));
    auto const is_fixed = (!!(option & VmMapOption::Fixed)) || is_fixed_noreplace;
    if (is_fixed) {
        if (m_subvmas.has_range(base_aligned, size_aligned)) {
            if (is_fixed_noreplace) {
                return ktl::err(Status::AlreadyExists);
            }

            // Overwrite the primitive regions.
            DEBUG_ASSERT(false, "Now we don't support to overwrite a group of existent pages");
        }
    } else {
        // If no given **Fixed** options, the specific address would be overwritten.
        // FIXME(SmallHuaZi) (base_ + size_) may causes an overflow error.
        auto result = alloc_spot(size_aligned, PAGE_SIZE, m_base, m_base + m_size - 1);
        if (!result) {
            return ktl::err(Status::InvalidArguments);
        }
    }

    return ktl::ok(ustl::make_pair(base_aligned, size_aligned));
}

auto VmArea::map_at(PhysAddr phys_base, VirtAddr *virt_base, usize size, MmuFlags mmuf,
                    VmMapOption options, char const *name) -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify(AREA_MAGIC);

    DEBUG_ASSERT(ustl::mm::is_aligned(phys_base, PAGE_SIZE));
    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return ustl::err(Status::InvalidArguments);
    }
    if (virt_base) {
        *virt_base = ustl::mm::align_down(*virt_base, PAGE_SIZE);
    }

    ustl::Arc<VmObjectPhysical> vmo;
    auto status = VmObjectPhysical::create(phys_base, size, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    return map_with_vmo(virt_base, size, mmuf, ustl::move(vmo), options, name);
}

auto VmArea::map(VirtAddr *base, usize size, MmuFlags mmuf, VmMapOption options, char const *name)
    -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify(AREA_MAGIC);

    DEBUG_ASSERT(!(options & VmMapOption::Fixed), "Now we don't support the overwritting option");
    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return ustl::err(Status::InvalidArguments);
    }
    if (base) {
        *base = ustl::mm::align_down(*base, PAGE_SIZE);
    }

    ustl::Arc<VmObjectPaged> vmo;
    auto status = VmObjectPaged::create(GAF_KERNEL, size, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    return map_with_vmo(base, size, mmuf, ustl::move(vmo), options, name);
}

auto VmArea::map_with_vmo(VirtAddr *base, usize size, MmuFlags mmuf, ustl::Arc<VmObject> vmo,
                          VmMapOption option, char const *name)
    -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify(AREA_MAGIC);

    VirtAddr vma_ofs = 0;
    if (base && *base >= m_base) {
        vma_ofs = *base - m_base;
    }

    ustl::Arc<VmMapping> mapping;
    auto status = create_mapping(vma_ofs, size, 0, mmuf, vmo, name, option, &mapping);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    // Do we immediately commit current mapping request?
    if (!!(VmMapOption::Commit & option)) {
        status = vmo->commit_range(0, size);
        if (Status::Ok != status) {
            return ustl::err(status);
        }

        status = mapping->map(0, size, true, MapControl::ErrorIfExisting);
        if (Status::Ok != status) {
            return ustl::err(status);
        }
    }

    if (base) {
        *base = mapping->base();
    }

    return ustl::ok(ustl::move(mapping));
}

auto VmArea::unmap(usize offset, usize size) -> Status {
    m_canary.verify(AREA_MAGIC);

    if (!is_active()) {
        return Status::BadState;
    }

    // Reject offsets that would wrap |m_base + offset| below.
    if (offset > m_size) {
        return Status::InvalidArguments;
    }

    auto [base, aligned_size] = resolve_page_range(m_base + offset, size);
    if (!aligned_size) {
        return Status::InvalidArguments;
    }

    if (!check_range(base, aligned_size)) {
        return Status::InvalidArguments;
    }

    Guard guard{get_lock()};
    return unmap_locked(base, aligned_size);
}

auto VmArea::unmap_locked(VirtAddr base, usize size) -> Status {
    VirtAddr const end = base + size;

    for (auto iter = m_subvmas.begin(); iter != m_subvmas.end();) {
        auto *const child = *iter;
        if (child->base() >= end) {
            // Children are ordered by base, so nothing further can overlap.
            break;
        }
        // Advance before disposal below invalidates the iterator.
        ++iter;

        if (child->end() <= base) {
            continue;
        }

        VirtAddr const overlap_base = std::max(child->base(), base);
        VirtAddr const overlap_end = std::min(child->end(), end);

        if (!child->is_mapping()) {
            // Unmap never destroys areas, it just descends into them.
            auto status = static_cast<VmArea *>(child)->unmap_locked(overlap_base,
                                                                     overlap_end - overlap_base);
            if (Status::Ok != status) {
                return status;
            }
            continue;
        }

        auto *mapping = static_cast<VmMapping *>(child);
        if (base <= child->base() && child->end() <= end) {
            // Fully covered, so the mapping has no role left to play: tear it
            // down for good instead of leaving an object whose every region is
            // unmapped.
            auto status = mapping->destroy();
            if (Status::Ok != status) {
                return status;
            }
        } else {
            auto status = mapping->unmap_locked(overlap_base - child->base(),
                                                overlap_end - overlap_base, UnmapControl::None);
            if (Status::Ok != status) {
                return status;
            }
        }
    }

    return Status::Ok;
}

auto VmArea::protect(usize offset, usize size, MmuFlags flags) -> Status {
    m_canary.verify(AREA_MAGIC);
    return Status::Unimplemented;
}

auto VmArea::reserve(usize vma_off, usize size_bytes, MmuFlags mmuf, char const *name) -> Status {
    m_canary.verify(AREA_MAGIC);
    auto [base, size] = resolve_page_range(m_base + vma_off, size_bytes);
    if (!size) {
        return Status::InvalidArguments;
    }

    if (!validate_mmuflags(mmuf)) {
        return Status::InvalidArguments;
    }

    // TODO(SmallHuaZi) Should we provide a specific way to make these kind of zero length VMO.
    ustl::Arc<VmObjectPaged> vmo;
    auto status = VmObjectPaged::create(GAF_KERNEL, 0, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    ustl::Arc<VmMapping> mapping;
    status = VmMapping::create(this, base, size, m_vmaf, ustl::move(vmo), 0, mmuf, name, &mapping);
    if (Status::Ok != status) {
        return status;
    }

    // The mappings are existing, just update the permission of them.
    status = m_aspace->arch_aspace().protect(base, size >> PAGE_SHIFT, mmuf);
    return status;
}

auto VmArea::destroy() -> Status {
    m_canary.verify(AREA_MAGIC);

    ustl::Arc<Self> curr(this);
    while (curr) {
        while (!curr->m_subvmas.empty()) {
            ustl::Arc<Base> aom(*curr->m_subvmas.begin());
            if (!aom->is_mapping()) {
                // If not a mapping, recursive to keep destroying.
                curr = ustl::adopt<Self, ustl::sync::ThreadSafePolicy>(aom.as_ptr_mut());
                continue;
            }

            auto status = aom->destroy();
            if (Status::Ok != status) {
                return status;
            }
        }

        // Destroy current node.
        auto *const parent = m_parent;
        curr->Base::destroy();
        if (curr.as_ptr() == this) {
            break;
        }

        // Traverse up.
        curr = parent ? ustl::adopt<Self, ustl::sync::ThreadSafePolicy>(parent) : ustl::Arc<Self>();
    }

    m_num_mappings = 0;
    return Status::Ok;
}

auto VmArea::contains(VirtAddr addr) const -> bool {
    // One subtraction rather than two comparisons: unsigned wrap turns an
    // address below the base into a huge offset, which fails the same test.
    return addr - m_base < m_size;
}

/// Finds the mapping that |va| falls in, descending through sub-areas.
///
/// Children are ordered by base address, so the scan can stop at the first one
/// starting past |va| instead of walking the whole set.
auto VmArea::find_mapping_locked(VirtAddr va) -> VmMapping * {
    m_canary.verify(AREA_MAGIC);

    if (!contains(va)) {
        return nullptr;
    }

    for (auto *child: m_subvmas) {
        if (child->base() > va) {
            break;
        }
        if (va - child->base() >= child->size()) {
            continue;
        }

        if (child->is_mapping()) {
            return static_cast<VmMapping *>(child);
        }
        return static_cast<VmArea *>(child)->find_mapping_locked(va);
    }

    return nullptr;
}

auto VmArea::dump_locked() const -> void {
    m_canary.verify(AREA_MAGIC);

    log::trace("VMA {}: [0x{:X}, 0x{:X}]", m_name, m_base, m_size);
    for (auto const *child: m_subvmas) {
        if (child->is_mapping()) {
            log::trace("  M {:<10}: [0x{:X}, 0x{:X}]", child->m_name, child->base(), child->size());
        } else {
            log::trace("  A {:<10}: [0x{:X}, 0x{:X}]", child->m_name, child->base(), child->size());
        }
    }
}

/// TODO(SmallHuaZi): should take the aspace lock and then defer, but the lock
/// is not reachable from here yet, and its current callers (init_vmm) do not
/// hold it. Left as a plain forward so the two spellings cannot drift apart.
auto VmArea::dump() const -> void {
    dump_locked();
}

} // namespace ours::mm