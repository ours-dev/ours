/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_OBJECT_PAGED_HPP
#define OURS_MM_VM_OBJECT_PAGED_HPP 1

#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/sync/mutex.hpp>

#include <logz4/log.hpp>
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

    FORCE_INLINE
    auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<VmCowPages::Cursor, Status> {
        return m_cow_pages->make_cursor(offset, size);
    }

    /// Priviate on logic, please go to use the facotry methods like VmObjectPaged::create*.
    VmObjectPaged(Flags vmof, ustl::Arc<VmCowPages>);
    virtual ~VmObjectPaged() = default;

  private:
    auto commit_range_internal(VirtAddr offset, usize size, CommitOptions option) -> Status;

    /// The page walk read(), write(), read_user() and write_user() share;
    /// |copy| moves the bytes between one page and the caller's buffer and says
    /// whether a user-side fault stopped it. Defined in the .cpp, the only place
    /// it is instantiated.
    template <typename Copy>
    auto read_write(VirtAddr offset, usize size, bool will_write, Copy const &copy) -> Status;

    ustl::Arc<VmCowPages> m_cow_pages;
    sync::Mutex m_mutex;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_OBJECT_PAGED_HPP