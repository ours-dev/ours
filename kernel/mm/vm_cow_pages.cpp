#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_cow_pages.hpp>

namespace ours::mm {
using VmCowPagesCache = ObjectCache::Instance<VmCowPages>;

VmCowPages::VmCowPages(Gaf gaf, usize nr_pages) : m_gaf(gaf), m_size(nr_pages) {}

VmCowPages::~VmCowPages() = default;

auto VmCowPages::create(Gaf gaf, usize size, ustl::Arc<VmCowPages> *out) -> Status {
    auto cow_pages = VmCowPagesCache::get(GAF_KERNEL, gaf, size);
    if (!cow_pages) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(cow_pages);

    return Status::Ok;
}

auto VmCowPages::make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status> {
    return ustl::ok(Cursor(this, offset, size));
}

auto VmCowPages::alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status {
    auto frame = alloc_frames(m_gaf, order);
    if (!frame) {
        return Status::OutOfMem;
    }

    // Fresh from the buddy allocator: declare the role, do not query it.
    frame->become<page::VmmBody>();
    *page = frame;
    return Status::Ok;
}

auto VmCowPages::commit_range_locked(VirtAddr offset, usize size, usize *nr_commited) -> Status {
    if (!size) {
        return Status::InvalidArguments;
    }

    if (offset + size > m_size) {
        return Status::InvalidArguments;
    }

    auto cursor = make_cursor(offset, size);
    if (!cursor) {
        return Status::InternalError;
    }

    auto status = Status::Ok;
    usize commited = 0;
    MultiPageRequest page_request;
    for (usize i = 0; i < size; i += PAGE_SIZE) {
        auto result = cursor->require_owned_page(1, &page_request);
        if (!result) {
            status = result.unwrap_err();
            DEBUG_ASSERT(status != Status::ShouldWait);
            break;
        }
        commited += 1;
    }

    if (nr_commited) {
        *nr_commited = commited;
    }

    return status;
}

/// The followings are in class VmCowPages::Cursor.

VmCowPages::Cursor::Cursor(VmCowPages *target, VirtAddr offset, usize sizer_pages)
    : m_target(target) {}

auto VmCowPages::Cursor::require_owned_page(usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<VmPage *> {

    // Replace page refernece with a real page.
    if (is_type_reference()) {
        auto const status = convert_reference_to_page(request->get_anonymous());
        if (Status::Ok != status) {
            return ktl::err(status);
        }
    }

    VmPage *page = nullptr;
    auto status = m_target->alloc_pages(0, &page, nullptr);
    if (Status::Ok == status) {
        return ustl::ok(page);
    }

    // No any available pages for current request, try to create asynchronous page request.
    return ustl::err(create_read_request(nr_pages, request->get_read()));
}

/// What the cursor is currently sitting on.
///
/// TODO(SmallHuaZi): VmPageMap does not distinguish content kinds yet, so
/// there are no reference or zero entries to report -- every populated slot
/// is a plain page. These answer accordingly rather than guessing, which
/// keeps require_owned_page() on its one working path.
auto VmCowPages::Cursor::is_type_reference() const -> bool {
    return false;
}

auto VmCowPages::Cursor::convert_reference_to_page(AnonymousPageRequest *request) -> Status {
    // Unreachable while is_type_reference() is always false.
    return Status::Unimplemented;
}

auto VmCowPages::Cursor::create_read_request(usize nr_pages, PageRequest *page_request) -> Status {
    return Status::Unimplemented;
}

auto VmCowPages::Cursor::create_dirty_request(usize nr_pages, PageRequest *page_request) -> Status {
    return Status::Unimplemented;
}

INIT_CODE static auto init_vm_cow_pages_cache() -> void {
    auto const status = VmCowPagesCache::init("vm-cow-pages-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmCowPages");
    }
    log::trace("VmCowPagesCache has been created");
}
// Heap, not PlatformEarly: this creates a MemCache, so it needs the object
// caches to be up.
GKTL_INIT_HOOK(VmCowPagesCacheInit, init_vm_cow_pages_cache, gktl::InitLevel::Heap);

} // namespace ours::mm