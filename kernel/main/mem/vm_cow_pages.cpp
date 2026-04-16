#include <ours/mem/vm_cow_pages.hpp>
#include <ours/mem/object-cache.hpp>
#include <ours/mem/pmm.hpp>

#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>

namespace ours::mem {
    static ObjectCache *s_vm_cow_pages_cache;

    VmCowPages::VmCowPages(Gaf gaf, usize nr_pages)
        : m_gaf(gaf),
          m_size(nr_pages)
    {}

    VmCowPages::~VmCowPages() = default;

    auto VmCowPages::create(Gaf gaf, usize size, ustl::Rc<VmCowPages> *out) -> Status {
        auto cow_pages = new (*s_vm_cow_pages_cache, kGafKernel) Self(gaf, size);
        if (!cow_pages) {
            return Status::OutOfMem;
        }
        *out = cow_pages;

        return Status::Ok;
    }

    auto VmCowPages::make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status> {
        return ustl::ok(Cursor(this, offset, size));
    }

    auto VmCowPages::alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status {
        auto frame = alloc_frame(m_gaf, order);
        if (!frame) {
            return Status::OutOfMem;
        }
        *page = role_cast<PageFrame::Role::Vmm>(frame);
        return Status::Ok;
    }

    auto VmCowPages::commit_range_locked(VirtAddr offset, usize size, ai_out usize *nr_commited) -> Status {
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
        auto commited = 0;
        PageRequest page_request;
        for (auto i = 0; i < size; i += PAGE_SIZE) {
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
        : m_target(target)
    {}

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
        auto status = m_target->alloc_pages(0, &page, 0);
        if (Status::Ok == status) {
            return ustl::ok(page);
        }

        // No any available pages for current request, try to create asynchronous page request.
        return ustl::err(create_read_request(nr_pages, page_request));
    }

    auto VmCowPages::Cursor::create_read_request(usize nr_pages, PageRequest *page_request) -> Status {
        return Status::Unimplemented;
    }

    auto VmCowPages::Cursor::create_dirty_request(usize nr_pages, PageRequest *page_request) -> Status {
        return Status::Unimplemented;
    }

    INIT_CODE
    static auto init_vm_cow_pages_cache() -> void {
        s_vm_cow_pages_cache = ObjectCache::create<VmCowPages>("vmo-paged-cache", OcFlags::Folio);
        if (!s_vm_cow_pages_cache) {
            panic("Failed to create object cache for VmArea");
        }
        log::trace("VmCowPagesCache has been created");
    }
    GKTL_INIT_HOOK(VmCowPagesCacheInit, init_vm_cow_pages_cache, gktl::InitLevel::PlatformEarly);

} // namespace ours::mem