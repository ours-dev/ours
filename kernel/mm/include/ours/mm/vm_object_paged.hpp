/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_OBJECT_PAGED_HPP
#define OURS_MM_VM_OBJECT_PAGED_HPP 1

#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/sync/mutex.hpp>

#include <logz4/log.hpp>
#include <ustl/function/function.hpp>
#include <ustl/result.hpp>

namespace ours::mm {
class VmObjectPaged: public VmObject {
    using Base = VmObject;
    using Self = VmObjectPaged;

  public:
    static auto create(Gaf gaf, usize size, Flags vmof, ustl::Arc<VmObjectPaged> *out) -> Status;

    static auto create_contiguous(Gaf gaf, usize size, Flags vmof, ustl::Arc<VmObjectPaged> *out)
        -> Status;

    ///
    auto commit_range(VirtAddr offset, usize size, CommitOptions option) -> Status override;

    ///
    auto decommit(VirtAddr offset, usize size) -> Status override;

    ///
    auto take_pages(VirtAddr, usize, VmPage::List *) -> Status override;

    ///
    auto supply_pages(VirtAddr, usize, VmPage::List *, SupplyOption) -> Status override;

    /// A page nobody has written reads as zero. Reading it commits a zeroed
    /// page, as a fault would (VmCowPages::Cursor::require_read_page()).
    auto read(void *out, VirtAddr offset, usize size) -> Status override;

    /// Commits every page the range touches.
    auto write(void const *in, VirtAddr offset, usize size) -> Status override;

    auto read_user(user_copy::UserOutPtr<u8> out, VirtAddr offset, usize size) -> Status override;

    auto write_user(user_copy::UserInPtr<u8> in, VirtAddr offset, usize size) -> Status override;

    /// Read without m_mutex: nothing can change a paged VMO's size, because
    /// Resizable has no implementation. The day it does, this has to take the
    /// lock, as Zircon's size_locked() requires its caller to.
    [[nodiscard]]
    auto size() const -> usize override {
        return m_cow_pages->size_locked();
    }

    /// The lock over this object's pages. A mapping takes it to fault a page
    /// in or to map a range, inside its address space's lock: the order is
    /// always address space, then object, as in Zircon, where
    /// VmMapping::PageFault() takes `Guard<CriticalMutex>{paged->lock()}` with
    /// the aspace lock already held. Nothing takes them the other way round;
    /// copy_page_by_page() lets go of this one before it resolves a user fault.
    TA_RETURN_CAPABILITY(m_mutex) FORCE_INLINE auto lock() -> sync::Mutex & {
        return m_mutex;
    }

    /// A cursor walks VmCowPages::m_pages and inserts into it, and m_mutex is
    /// the one lock that tree has. The fault path used to build one under the
    /// address space's lock alone, which is how the tree came to be written
    /// under two unrelated locks; TA_REQUIRES makes that a compile error.
    TA_REQUIRES(m_mutex)
    FORCE_INLINE
    auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<VmCowPages::Cursor, Status> {
        return m_cow_pages->make_cursor(offset, size);
    }

    /// Priviate on logic, please go to use the facotry methods like VmObjectPaged::create*.
    VmObjectPaged(Flags vmof, ustl::Arc<VmCowPages>);
    virtual ~VmObjectPaged() = default;

  private:
    auto commit_range_internal(VirtAddr offset, usize size, CommitOptions option) -> Status;

    /// Moves |chunk| bytes between |page| and the caller's buffer at |done|,
    /// and says whether a user-side fault stopped it. |page| is writable even
    /// for a read, so that one type serves both directions; the read copies
    /// take it as `u8 const *`.
    using CopyPage =
        ustl::function::Function<auto(u8 *page, usize done, usize chunk)->UserCopyCaptureResult>;

    /// Copies [offset, offset + size) one page at a time, handing |copy| each
    /// page and the matching slice of the caller's buffer -- the page walk
    /// read(), write(), read_user() and write_user() share. Named for what it
    /// does rather than after Zircon's ReadWriteInternal(), which it mirrors.
    ///
    /// |copy| is a Function rather than a template parameter. As a template the
    /// walk -- range check, cursor, retry loop, livelock check -- was
    /// instantiated once per caller, four copies of the same code, to save one
    /// indirect call per page when each call copies up to a page. The trade
    /// ran the wrong way, so it is one walk and one indirect call now.
    auto copy_page_by_page(VirtAddr offset, usize size, bool will_write, CopyPage const &copy)
        -> Status;

    ustl::Arc<VmCowPages> m_cow_pages;
    sync::Mutex m_mutex;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_OBJECT_PAGED_HPP