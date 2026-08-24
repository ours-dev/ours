#include <ktl/kmalloc.hpp>
#include <ours/assert.hpp>
#include <ours/mm/object-cache.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/platform/init.hpp>
#include <ustl/bit.hpp>

using namespace ours::mm;

namespace ktl {
constexpr static auto const kNumMallocCaches = 10;

constexpr static auto const kMallocMinOrder = 3;

constexpr static auto const kMinMallocSize = sizeof(usize);

constexpr static auto const kMaxMallocSize = BIT(kNumMallocCaches + kMallocMinOrder);

static auto get_allocation_index(usize size) -> usize {
    if (!size) {
        return 0;
    }

    if (size <= kMaxMallocSize) {
        return ustl::bit_width<usize>(size - 1) - kMallocMinOrder;
    }

    DEBUG_ASSERT(false, "Unreachable");
}

static ustl::Array<ObjectCache, kNumMallocCaches> s_kmalloc_cache;

auto kmalloc(usize size, Gaf gaf, NnId nid) -> void * {
    if (size > kMaxMallocSize) {
        // Now we do not support to allocate frames from kmalloc
        return nullptr;
    }
    if (nid == NUMA_MAX_NODE) {
        nid = current_node();
    }

    auto const index = get_allocation_index(size);
    return s_kmalloc_cache[index].do_allocate(gaf, nid);
}

auto kmalloc(usize size, Gaf gaf) -> void * {
    if (size > kMaxMallocSize) {
        // Now we do not support to allocate frames from kmalloc
        return nullptr;
    }

    auto const index = get_allocation_index(size);
    return s_kmalloc_cache[index].do_allocate(gaf, current_node());
}

auto kfree(void *object) -> void {
    auto slab = role_cast<PageFrame::Role::Slab>(virt_to_folio(object));
    DEBUG_ASSERT(slab && slab->object_cache);
    slab->object_cache->do_deallocate(slab, object);
}

INIT_CODE
auto init_kmalloc() -> void {
    char name[16];
    for (auto i = 0; i < s_kmalloc_cache.size(); ++i) {
        auto status = s_kmalloc_cache[i].init(name, BIT(i + kMallocMinOrder), alignof(usize), {});
        DEBUG_ASSERT(Status::Ok == status);
    }
}

} // namespace ktl
