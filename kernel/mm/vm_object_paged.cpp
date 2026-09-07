#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ustl/mem/align.hpp>

using ustl::mm::align_down;
using ustl::mm::align_up;

namespace ours::mm {
using VmoPagedCache = ObjectCache::Instance<VmObjectPaged>;

VmObjectPaged::VmObjectPaged(VmoFlags vmof, ustl::Arc<VmCowPages> cowpages)
    : Base(gktl::magic<Self>(), Type::Paged, vmof), m_cow_pages(ustl::move(cowpages)) {}

auto VmObjectPaged::create(Gaf gaf, usize size, VmoFlags vmof, ustl::Arc<VmObjectPaged> *out)
    -> Status {
    // Check vmof
    ustl::Arc<VmCowPages> cow_pages;
    auto status = VmCowPages::create(gaf, size, &cow_pages);
    if (Status::Ok != status) {
        return status;
    }

    auto vmo = VmoPagedCache::get(GAF_KERNEL, vmof, ustl::move(cow_pages));
    if (!vmo) {
        return Status::OutOfMem;
    }

    /// A non-lazy VMO need we directly request pages it demands.
    if (!(vmof & VmoFlags::Lazy)) {
        CommitOptions options{};
        if (!!(vmof & VmoFlags::Pinned)) {
            options |= CommitOptions::Pin;
        }

        vmo->commit_range(0, size, options);
    }
    *out = ustl::Arc<Self>::adopt(vmo);

    return Status::Ok;
}

FORCE_INLINE auto VmObjectPaged::commit_range_internal(VirtAddr offset, usize size,
                                                       CommitOptions option) -> Status {
    if (!!(option & CommitOptions::Pin)) {
        if (!size) {
            return Status::InvArgs;
        }
    }

    if (!size) {
        return Status::Ok;
    }

    Guard guard{m_mutex};
    if (m_cow_pages->size_locked() < size) {
        return Status::OutOfRange;
    }

    for (usize i = 0; i < size; i += PAGE_SIZE) {
        usize nr_commited = 0;
        auto status = m_cow_pages->commit_range_locked(offset, size, &nr_commited);

        if (status != Status::Ok && status != Status::ShouldWait) {
            return status;
        }
    }

    return Status::Ok;
}

auto VmObjectPaged::commit_range(VirtAddr offset, usize size, CommitOptions option) -> Status {
    m_canary.verify<Self>();

    log::trace("VMO Commit Action: [ofs: {:X}, size: {:X}]", offset, size);
    return commit_range_internal(offset, size, option);
}

auto VmObjectPaged::decommit(VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::take_pages(VirtAddr offset, usize size, VmPage::List *pages) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::supply_pages(VirtAddr offset, usize size, VmPage::List *pages,
                                 SupplyOption options) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::read(void *out, VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::write(void *in, VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

INIT_CODE static auto init_vmo_paged_cache() -> void {
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