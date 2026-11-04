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

    static auto create(Gaf alloc_flags, usize size, Flags vmof, ustl::Arc<VmObjectPaged> *out)
        -> Status;

    /// A VMO of |size| bytes, rounded up to whole pages, whose pages are one run
    /// of physical memory starting on a multiple of 2^|alignment_log2| bytes,
    /// for a device to read and write as one buffer: a ring, say, that the
    /// device is given one address of. Every page is there from the start,
    /// zero, and none is ever reclaimed, as a Pinned object's are not; they are
    /// not pinned in the sense of pin_range(), which counts, so the VMO is
    /// pinned for a device in the usual way (BtiDispatcher::pin()).
    ///
    /// One block of the buddy allocator, so the run is the size of a power of
    /// two pages, aligned to itself, and what is asked for beyond |size| is
    /// given back page by page. The block is as big as the size needs, or as
    /// the alignment does if that is bigger: a 2-page buffer on 64 KiB is a
    /// 16-page block with 14 pages returned. It cannot be bigger than the
    /// allocator's largest block (MAX_FRAME_ORDER), 4 MiB: past it, and when
    /// no block of the size is free, the answer is NoMemory.
    ///
    /// |alignment_log2| 0 means a page. A value below the page's own is InvArgs,
    /// and so is one that does not fit a shift, and an empty |size|. OutOfRange
    /// for a |size| past MAX_SIZE. They are asked in that order, InvArgs, then
    /// OutOfRange, then NoMemory. |vmof| is for the future: Lazy and LargePages
    /// say the opposite of what this is, and are InvArgs.
    ///
    /// |alloc_flags| must not carry CanWait: the call answers NoMemory where a caller
    /// that waits would sleep. The run can be anywhere in memory; nothing here
    /// limits it to what a device with a 32-bit address can reach.
    static auto create_contiguous(Gaf alloc_flags, usize size, usize alignment_log2, Flags vmof,
                                  ustl::Arc<VmObjectPaged> *out) -> Status;

    /// A VMO of |size| bytes whose pages come from |source| rather than being
    /// zero: a fault on a page nobody has supplied raises a request against the
    /// source and sleeps until the provider supplies it (supply_pages()). See
    /// VmCowPages::create_external().
    ///
    /// Nothing is committed, whatever |vmof| says: there is nothing to commit
    /// until the provider supplies it. So Pinned, which promises that no access
    /// faults, is InvArgs. On any failure the source is closed, as the VMO
    /// would have closed it.
    static auto create_external(ustl::Arc<PageSource> source, Gaf alloc_flags, usize size,
                                Flags vmof, ustl::Arc<VmObjectPaged> *out) -> Status;

    /// A VMO that takes over |size| bytes of frames at |base|, reserved since
    /// boot, as its own pages: no allocation and no copy, and when the VMO
    /// dies the frames go to the buddy like any page it allocated. The boot
    /// image is handed to userboot this way.
    ///
    /// Both ends must be page-aligned (InvArgs). Every frame must be reserved,
    /// and each is taken off the reserved books (unwire_frame()) and added
    /// (VmCowPages::add_new_page_locked()), in that order; a frame that is
    /// not reserved, or a page list that cannot hold them all, is a panic:
    /// this runs once, at boot, for memory nothing else can use.
    /// Neither Lazy nor Pinned: every page is there from the start, and
    /// nothing pins them.
    ///
    /// |alloc_flags| is what the VMO allocates with afterwards -- a copy-on-write
    /// clone of it inherits it -- so a VMO user space can reach takes the user
    /// class, as create()'s callers choose theirs.
    static auto create_from_wired_pages(Gaf alloc_flags, PhysAddr base, usize size,
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

    auto commit_range(usize offset, usize size, CommitOptions option) -> Status override;

    ///
    auto decommit(usize offset, usize size) -> Status override;

    /// See VmObject::pin_range(). Commits the range as commit_range() does, one
    /// page source wait at a time, and pins what it committed; a failure part
    /// way undoes the pins already made, so the range is pinned whole or not
    /// at all. While any page of the object holds a pin, create_clone() and
    /// take_pages() are BadState: both would move or share a pinned page.
    auto pin_range(usize offset, usize size) -> Status override;

    /// See VmObject::unpin_range().
    auto unpin_range(usize offset, usize size) -> Status override;

    /// See VmObject::lookup_contiguous(). A page this object only sees through
    /// an ancestor counts as there, at the ancestor's frame.
    auto lookup_contiguous(usize offset, usize size, PhysAddr *out) -> Status override;

    /// Takes [offset, offset + size) of this object's slots into |pages|, a
    /// fresh list of size / PAGE_SIZE slots (see VmCowPages::take_pages_locked()).
    /// Page aligned and the list's length, InvArgs otherwise; OutOfRange past
    /// the end; BadState for a Pinned object, whose pages the kernel's own
    /// mappings rely on staying put; NotSupported for an object with a page
    /// source. A clone's range reads zero afterwards, and its siblings still
    /// see what they saw.
    auto take_pages(usize, usize, VmPageSpliceList *) -> Status override;

    /// Puts the slots of |pages| at [offset, offset + size) of an object made
    /// by create_external(), and wakes the faults waiting on them (see
    /// VmCowPages::supply_pages_locked()). Page aligned, an unread list of
    /// size / PAGE_SIZE slots, InvArgs otherwise; OutOfRange past the end.
    /// Only SupplyOption::AutoRelease: a slot already full keeps what it has
    /// and the supplied page is freed. TransferData, which would replace it,
    /// is NotSupported.
    auto supply_pages(usize, usize, VmPageSpliceList *, SupplyOption) -> Status override;

    /// See VmCowPages::detach_source_locked(). Every object over the same
    /// pages, a slice included, is detached with it: the source is the pages'.
    auto detach_source() -> void override;

    [[nodiscard]]
    auto source_owner_id() const -> ustl::Option<u64> override;

    /// A page nobody has written reads as zero. Reading it commits a zeroed
    /// page, as a fault would (VmCowPages::Cursor::require_page()). In a
    /// VMO made by create_external() there is no such page: the read waits until
    /// the provider supplies it, and fails if the provider fails it. The same
    /// holds for write(), read_user() and write_user().
    auto read(void *out, usize offset, usize size) -> Status override;

    auto store_u64(usize offset, u64 value) -> Status override;

    /// The pages of [offset, offset + len) of this object, a slice's window
    /// included, that its own node of the page tree holds. A range past the
    /// end counts as far as the end.
    [[nodiscard]]
    auto committed_pages(u64 offset, u64 len) -> usize override;

    /// Commits every page the range touches.
    auto write(void const *in, usize offset, usize size) -> Status override;

    auto read_user(user_copy::UserOutPtr<u8> out, usize offset, usize size) -> Status override;

    auto write_user(user_copy::UserInPtr<u8 const> in, usize offset, usize size) -> Status override;

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
    auto make_cursor(usize offset, usize size) -> ustl::Result<VmCowPages::Cursor, Status> {
        if (size > m_size || offset > m_size - size) {
            return ustl::err(Status::OutOfRange);
        }
        assert_cow_pages_locked();
        return ustl::ok(m_cow_pages->make_cursor(m_cow_offset + offset, size));
    }

    /// Whether this object's 2 MiB at |offset| are one block of its pages; see
    /// VmCowPages::find_block_locked(). The range is checked against this
    /// object as make_cursor() checks it, so a slice never answers for the
    /// pages past its end.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto find_block_locked(usize const offset)
        -> ustl::Option<VmCowPages::FoundBlock> {
        constexpr usize BLOCK = VmCowPages::BLOCK_SIZE;
        if (BLOCK > m_size || offset > m_size - BLOCK) {
            return ustl::NONE;
        }
        assert_cow_pages_locked();
        m_cow_pages->lock().assert_held();
        return m_cow_pages->find_block_locked(m_cow_offset + offset);
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

    auto commit_range_internal(usize offset, usize size, VmCowPages::CommitMode mode) -> Status;

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
    auto copy_page_by_page(usize offset, usize size, bool will_write, CopyPage const &copy)
        -> Status;

    /// Applies |op| to the page tables of this object's mappings of the part
    /// of [cow_offset, cow_offset + len) of the pages that it shows, given in
    /// the pages' bytes and translated into this object's. Called by the node,
    /// for each object over it: VmCowPages::operate_range_locked().
    TA_REQUIRES(m_lock)
    auto operate_cow_range_locked(u64 cow_offset, u64 len, RangeOp op) -> void;

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