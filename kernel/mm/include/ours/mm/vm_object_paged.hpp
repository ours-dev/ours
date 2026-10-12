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
    /// The largest size a paged VMO may have, in bytes: the last whole page
    /// below the top of the 64-bit range, less one more page, so that no
    /// offset or size inside a VMO can be rounded up to a page, or have a page
    /// added to it, without overflowing. What vmo_create checks a request
    /// against.
    ///
    /// The bound is the arithmetic, not the page tree's reach: the tree is
    /// keyed by 64-bit page offsets and reaches further, so the arithmetic is
    /// the tighter bound.
    constexpr static u64 MAX_SIZE = (~u64(0) & ~u64(PAGE_SIZE - 1)) - PAGE_SIZE;
    static_assert(MAX_SIZE % PAGE_SIZE == 0);
    static_assert(MAX_SIZE + PAGE_SIZE > MAX_SIZE, "a page more than MAX_SIZE must not wrap");

    static auto create(Gaf gaf, usize size, Flags vmof, ustl::Arc<VmObjectPaged> *out) -> Status;

    static auto create_contiguous(Gaf gaf, usize size, Flags vmof, ustl::Arc<VmObjectPaged> *out)
        -> Status;

    /// A VMO that takes over |size| bytes of frames at |base|, reserved since
    /// boot, as its own pages: no allocation and no copy, and when the VMO
    /// dies the frames go to the buddy like any page it allocated. The boot
    /// image is handed to userboot this way.
    ///
    /// Both ends must be page-aligned (InvArgs). Every frame must be reserved,
    /// and each is taken off the reserved books (unwire_frame()) and added
    /// (VmCowPages::add_new_page_locked()), in that order; a frame that is
    /// not reserved, or a page map that cannot hold them all, is a panic:
    /// this runs once, at boot, for memory nothing else can use.
    /// Neither Lazy nor Pinned: every page is there from the start, and
    /// nothing pins them.
    ///
    /// |gaf| is what the VMO allocates with afterwards -- a copy-on-write
    /// clone of it inherits it -- so a VMO user space can reach takes the user
    /// class, as create()'s callers choose theirs.
    static auto create_from_wired_pages(Gaf gaf, PhysAddr base, usize size,
                                        ustl::Arc<VmObjectPaged> *out) -> Status;

    ///
    /// A slice of this object: [offset, offset + size) of it, as an object of
    /// its own, over the very same pages. A write through either is seen
    /// through the other; the child has no pages and no lock of its own
    /// (VmCowPages::lock()), so a fault through it commits into the tree the
    /// parent reads.
    ///
    /// InvArgs unless both ends are page-aligned and the range lies wholly
    /// inside this object; a slice of a slice is a slice of the first
    /// object's pages at the sum of the offsets.
    auto create_slice(u64 offset, u64 size, ustl::Arc<VmObjectPaged> *out) -> Status;

    /// A copy-on-write clone of [offset, offset + size) of this object, as an
    /// object of its own: it reads what this one holds there, and neither ever
    /// sees the other's writes after the clone. Nothing is copied; the pages
    /// are shared until one side writes one, and that side then gets a page of
    /// its own. Write is taken from every mapping of the pages, this object's
    /// and every other object's over them, so that the first write faults.
    ///
    /// |offset| and |size| must be page aligned and |offset| no further than the
    /// end of this object, else InvArgs; |size| may be more than what is left,
    /// and the clone reads zero past this object's end, and never the bytes of
    /// the pages past a slice's window. BadState for an object with a pinned
    /// page (Pinned), whose pages cannot be shared: pinning is the object's
    /// and not the pages', which is why the refusal is here.
    ///
    /// The clone is not resizable and has no pages of its own until it writes;
    /// its flags are this object's.
    ///
    /// Its sibling for a window with no copy-on-write is create_slice().
    auto create_clone(u64 offset, u64 size, ustl::Arc<VmObjectPaged> *out) -> Status;

    /// The pages this object is over, for an operation that acts on the tree of
    /// them: the clone. Not for reaching into the pages, which are under the
    /// lock this object hands out.
    [[nodiscard]]
    auto cow_pages() const -> ustl::Arc<VmCowPages> const & {
        return m_cow_pages;
    }

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

    /// The pages of [offset, offset + len) of this object, a slice's window
    /// included, that its own node of the page tree holds. A range past the
    /// end counts as far as the end.
    [[nodiscard]]
    auto committed_pages(u64 offset, u64 len) -> usize override;

    /// Commits every page the range touches.
    auto write(void const *in, VirtAddr offset, usize size) -> Status override;

    auto read_user(user_copy::UserOutPtr<u8> out, VirtAddr offset, usize size) -> Status override;

    auto write_user(user_copy::UserInPtr<u8 const> in, VirtAddr offset, usize size)
        -> Status override;

    /// Read without m_lock: nothing can change a paged VMO's size, because
    /// Resizable has no implementation. The day it does, this has to take the
    /// lock, or require its caller to hold it.
    ///
    /// This object's size, not its pages': a slice sees only its window.
    [[nodiscard]]
    auto size() const -> usize override {
        return m_size;
    }

    /// A cursor walks VmCowPages::m_pages and inserts into it, and m_lock --
    /// the object's lock, VmObject::lock(), which is the VmCowPages's own --
    /// is the one lock that tree has. The fault path used to build one under
    /// the address space's lock alone, which is how the tree came to be
    /// written under two unrelated locks; TA_REQUIRES makes that a compile
    /// error.
    ///
    /// |offset| is in this object's bytes; the cursor walks the shared pages
    /// at m_cow_offset past it. The range is checked against this object's
    /// size first, because nothing below would: the page tree is the parent's
    /// too, and a mapping of a slice that ran past the slice's end would
    /// otherwise reach the parent's pages beyond it.
    TA_REQUIRES(m_lock)
    FORCE_INLINE
    auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<VmCowPages::Cursor, Status> {
        if (size > m_size || offset > m_size - size) {
            return ustl::err(Status::OutOfRange);
        }
        assert_cow_pages_locked();
        return m_cow_pages->make_cursor(m_cow_offset + offset, size);
    }

    /// Priviate on logic, please go to use the facotry methods like VmObjectPaged::create*.
    ///
    /// |cow_offset| and |size| are the window of |cowpages| this object is:
    /// 0 and the pages' size for an object created on its own pages, a range
    /// inside them for a slice.
    VmObjectPaged(Flags vmof, ustl::Arc<VmCowPages> cowpages, u64 cow_offset, usize size);
    ~VmObjectPaged() override;

  private:
    friend class VmCowPages;

    /// Tells the analysis that m_cow_pages's lock is held, given m_lock is.
    ///
    /// The two are one mutex -- the constructor hands the VmCowPages's lock to
    /// the base -- but the analysis knows them by two names, VmObject::m_lock
    /// and VmCowPages::m_lock, and cannot see that one is a reference to the
    /// other. This is where the code says so: it requires the first and
    /// asserts the second, and in a debug build checks that the mutex is in
    /// fact held. Called right before each use of m_cow_pages that requires
    /// its lock. It does not also return the pages, because the analysis
    /// cannot name the lock of an object a call returned, so the assertion has
    /// to stand on its own.
    TA_REQUIRES(m_lock)
    TA_ASSERT(m_cow_pages->lock()) FORCE_INLINE auto assert_cow_pages_locked() -> void {
        m_cow_pages->lock().assert_held();
    }

    auto commit_range_internal(VirtAddr offset, usize size, CommitOptions option) -> Status;

    /// Moves |chunk| bytes between |page| and the caller's buffer at |done|,
    /// and says whether a user-side fault stopped it. |page| is writable even
    /// for a read, so that one type serves both directions; the read copies
    /// take it as `u8 const *`.
    using CopyPage =
        ustl::function::Function<auto(u8 *page, usize done, usize chunk)->UserCopyCaptureResult>;

    /// Copies [offset, offset + size) one page at a time, handing |copy| each
    /// page and the matching slice of the caller's buffer -- the page walk
    /// read(), write(), read_user() and write_user() share.
    ///
    /// |copy| is a Function rather than a template parameter. As a template the
    /// walk -- range check, cursor, retry loop, livelock check -- was
    /// instantiated once per caller, four copies of the same code, to save one
    /// indirect call per page when each call copies up to a page. The trade
    /// ran the wrong way, so it is one walk and one indirect call now.
    auto copy_page_by_page(VirtAddr offset, usize size, bool will_write, CopyPage const &copy)
        -> Status;

    /// Takes Write away from the page tables of this object's mappings of the
    /// part of [cow_offset, cow_offset + len) of the pages that it shows, given
    /// in the pages' bytes and translated into this object's. Called by the
    /// node, for each object over it: VmCowPages::remove_write_locked().
    TA_REQUIRES(m_lock)
    auto remove_write_of_cow_range_locked(u64 cow_offset, u64 len) -> void;

    ustl::Arc<VmCowPages> m_cow_pages;

    /// Where this object's byte 0 sits in m_cow_pages, and how many bytes it
    /// has. Every offset into the pages goes through the first, every range
    /// check through the second. The window lives on the object, not on the
    /// pages, because a slice shares one VmCowPages with the object it was cut
    /// from.
    u64 const m_cow_offset;
    usize const m_size;

    /// How the pages know this object is over them, so that an operation on
    /// the pages can reach this object's mappings. Registered when the object
    /// is made and taken off when it dies.
    VmCowPages::Attachment m_attachment;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_OBJECT_PAGED_HPP