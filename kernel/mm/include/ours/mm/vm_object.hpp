/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_OBJECT_HPP
#define OURS_MM_VM_OBJECT_HPP

#include <ours/mm/types.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ktl/name.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/option.hpp>

namespace ours::mm {
class VmObject: public ustl::RefCounted<VmObject, ustl::sync::ThreadSafePolicy> {
    using Self = VmObject;

  public:
    class Builder;

    /// Which concrete VmObject this is, for the downcast in `as<Derived>()`.
    ///
    /// Plain values on purpose. These used to be `gktl::magic("VmObjectPaged")`
    /// and friends, because the type tag and the canary were the same field:
    /// the constructor seeded `m_canary` from this enum, and `type()` read the
    /// tag back out of the canary. That made one word do two jobs and neither
    /// could be checked -- a canary is only worth having if it is compared
    /// against something the object could not have produced by accident, and a
    /// type tag has no business being a corruption sentinel. It also broke
    /// outright: `VmObjectPaged` verified with `verify<Self>()`, which hashes
    /// the *type name*, against a canary holding the hash of a *string
    /// literal*. See the constructor.
    enum class Type {
        Paged,
        Physical,
    };

    enum class Flags : u8 {
        /// Can not be swaped out.
        Pinned = 0_bit,

        /// Never pre-commit pages, only when the page access fault occurs.
        Lazy = 1_bit,

        ///
        Resizable = 2_bit,

        /// A paged VMO whose holes are filled a 2 MiB block at a time where
        /// one fits, so that its mappings can use 2 MiB leaves (decision D2
        /// of .agents/tasks/large-pages: kernel-internal, not offered to
        /// programs until the measurement says whether it should be the
        /// default). See VmCowPages::Flags::LargeBlocks.
        LargePages = 3_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    enum class CommitOptions {
        /// No any effect, just for semantics.
        None,

        /// The pages mapped will not be swaped out.
        Pin = 0_bit,

        /// Map imediately but wait a page access fault come.
        Write = 1_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(CommitOptions);

    /// Commit the given range [offset, offset + size) to address space with |options|.
    ///
    /// Return Status::Ok if successfully commited. Otherwise failed to.
    virtual auto commit_range(VirtAddr offset, usize size,
                              CommitOptions options = CommitOptions::None) -> Status {
        return Status::Unimplemented;
    }

    /// Decommit the given range [offset, offset + size) from address space.
    /// However whether the range was mapped.
    ///
    /// Return Status::Ok if successfully decommited. Otherwise failed to.
    virtual auto decommit(VirtAddr offset, usize size) -> Status {
        return Status::Unimplemented;
    }

    /// Takes the slots of [offset, offset + size) out of this object into
    /// |pages|, in order -- its pages, its zero markers, and a gap for each
    /// page it has nothing at -- and unmaps them from every mapping first.
    /// What the object has there afterwards is nothing. |pages| has to be a
    /// fresh list of exactly size / PAGE_SIZE slots. See VmPageSpliceList.
    virtual auto take_pages(VirtAddr offset, usize size, VmPageSpliceList *pages) -> Status {
        return Status::Unsupported;
    }

    enum class SupplyOption : u8 {
        /// Overwrite those pages which already exists.
        TransferData = 0_bit,

        /// If the range already exists, release the pages supplied.
        AutoRelease = 1_bit,

        Default = AutoRelease,
    };
    /// Using enum bitflags for the extension in future.
    USTL_ENABLE_INNER_ENUM_BITMASK(SupplyOption);

    /// Stops this object's pages coming from its page source: the waiters on
    /// pages not supplied fail, and so does every later fault on one. What was
    /// supplied stays. Nothing for an object with no source.
    virtual auto detach_source() -> void {}

    /// The owner id of the provider behind this object's page source, none
    /// for an object with no source: how a pager tells that a VMO is one it
    /// made (PageProvider::owner_id()).
    [[nodiscard]]
    virtual auto source_owner_id() const -> ustl::Option<u64> {
        return ustl::NONE;
    }

    /// Puts the slots of |pages|, in order, at [offset, offset + size) of an
    /// object whose content comes from a page source: what the source's
    /// provider answers a request with. |pages| has to be unread and exactly
    /// size / PAGE_SIZE slots long.
    virtual auto supply_pages(VirtAddr offset, usize size, VmPageSpliceList *pages,
                              SupplyOption = SupplyOption::Default) -> Status {
        return Status::Unsupported;
    }

    /// Copies |size| bytes starting at byte |offset| of this object into the
    /// kernel buffer |outbuf|.
    ///
    /// Kernel buffers only: the copy runs under the object's lock and must not
    /// fault. A user buffer goes through read_user(). A range that does not lie
    /// wholly inside the object is OutOfRange and nothing is copied.
    virtual auto read(void *outbuf, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

    /// Copies |size| bytes from the kernel buffer |inbuf| into this object,
    /// starting at byte |offset|. The same rules as read().
    virtual auto write(void const *inbuf, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

    /// Stores |value| at |offset| as one aligned 8-byte store.
    ///
    /// write() copies a byte at a time, and a sequence word published for
    /// another CPU to load has to change in one store: a torn word can look
    /// even while the other words do not agree. |offset| not a multiple of 8
    /// is InvArgs. Unsupported for an object that is not paged.
    virtual auto store_u64(VirtAddr offset, u64 value) -> Status {
        return Status::Unsupported;
    }

    /// Copies |size| bytes starting at byte |offset| of this object straight
    /// into the user buffer |out|, with no kernel buffer in between.
    ///
    /// The copy runs under the object's lock, so a page fault on the user side
    /// is captured rather than resolved in place, and the lock is dropped to
    /// resolve it; see VmObjectPaged::copy_page_by_page(). InvArgs when some page of
    /// the user buffer cannot be made present, OutOfRange as read() has it.
    /// The user bytes before a failure may have been written.
    ///
    /// Unsupported by default, which is where a physical VMO stays.
    virtual auto read_user(user_copy::UserOutPtr<u8> out, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

    /// Copies |size| bytes from the user buffer |in| straight into this object,
    /// starting at byte |offset|. The same rules as read_user().
    virtual auto write_user(user_copy::UserInPtr<u8 const> in, VirtAddr offset, usize size)
        -> Status {
        return Status::Unsupported;
    }

    /// The object's size in bytes, which is the bound read(), write() and
    /// commit_range() check a range against.
    [[nodiscard]]
    virtual auto size() const -> usize = 0;

    virtual auto dirty_pages(VirtAddr offset, usize size) -> Status {
        return Status::Unimplemented;
    }

    virtual auto write_back_preamble(VirtAddr offset, usize size) -> Status {
        return Status::Unimplemented;
    }

    virtual auto write_back_postamble(VirtAddr offset, usize size) -> Status {
        return Status::Unimplemented;
    }

    FORCE_INLINE
    auto commit_range_pinned(usize offset, usize len, bool write) -> Status {
        auto options = CommitOptions::Pin;
        if (write) {
            options |= CommitOptions::Write;
        }
        return commit_range(offset, len, options);
    }

    FORCE_INLINE
    auto set_name(char const *name) -> void {
        // set() refuses only a null name, which it records as empty; a long
        // one is cut short, and a name is only ever read in dumps.
        auto _ = m_name.set(name);
    }

    /// The name into |out|, NUL-terminated; empty when it has none.
    FORCE_INLINE
    auto get_name(char *out, usize len) const -> void {
        m_name.get(out, len);
    }

    FORCE_INLINE
    auto type() const -> Type {
        return m_type;
    }

    /// What the object was created with.
    [[nodiscard]]
    FORCE_INLINE
    auto flags() const -> Flags {
        return m_flags;
    }

    /// The koid of the VMO object that wraps this one for userspace, or 0
    /// while none does: how a process's maps name the VMO behind a mapping.
    /// Written once, by set_user_id(), before the wrapper's first handle
    /// exists; relaxed, since a reader reaches the object only through a
    /// mapping made after that, under locks that order the two.
    [[nodiscard]]
    FORCE_INLINE
    auto user_id() const -> u64 {
        return m_user_id.load(std::memory_order_relaxed);
    }

    FORCE_INLINE
    auto set_user_id(u64 const user_id) -> void {
        m_user_id.store(user_id, std::memory_order_relaxed);
    }

    /// How many pages of [offset, offset + len) this object holds itself now:
    /// not the pages it would read from a parent it was cloned from, nor
    /// those no one has written. Takes the object's lock, so a caller may
    /// hold an address space's lock (the documented order) but not this one.
    /// None for an object without pages of its own.
    [[nodiscard]]
    virtual auto committed_pages(u64 offset, u64 len) -> usize {
        return 0;
    }

    /// The object's lock: over its pages (a paged VMO's page tree) and over the
    /// list of mappings of it. One lock for both, because an operation that
    /// changes pages -- a decommit, say -- has to reach every mapping of them
    /// while no page and no mapping can change under it.
    ///
    /// Taken inside an address space's lock, never the other way round:
    /// VmMapping::page_fault(), map_paged(), activate() and destroy() all run
    /// with the aspace lock held and take this one inside it, and
    /// VmObjectPaged::copy_page_by_page() lets go of this one before it
    /// resolves a user fault.
    ///
    /// The mutex is not the object's own. A paged VMO's is its VmCowPages's,
    /// shared with every object over the same pages (a slice and its parent);
    /// a physical VMO, which has no pages to share, keeps one of its own. The
    /// derived class says which by handing it to the constructor.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto lock() -> sync::Mutex & {
        return m_lock;
    }

    /// Makes |change| to the page tables of every mapping of
    /// [offset, offset + len) of this object -- offsets in this object's own
    /// bytes -- and leaves each mapping's record of what it allows alone, so
    /// that the next access to the page faults and is resolved instead of being
    /// refused.
    ///
    /// For an operation that is about to make pages shared (the clone takes
    /// Write: a page that another node is about to see must not be writable
    /// through a mapping that will not know), or take them out of the object
    /// (take_pages() unmaps them: an entry left behind would reach a frame the
    /// object no longer has). Held under the object's lock and not the address
    /// space's, which is what the mappings' records are written under
    /// (VmMapping::protect_locked()) so that this can read them.
    TA_REQUIRES(m_lock)
    auto change_range_locked(u64 offset, u64 len, RangeChange change) -> void;

  protected:
    friend class VmMapping;

    // RefCounted<VmObject,...>::destroy() calls ~VmObject() through the base
    // class on the final release(); without this friend, that call is
    // ill-formed since the destructor below is protected.
    friend class ustl::RefCounted<VmObject, ustl::sync::ThreadSafePolicy>;

    template <typename Derived>
    constexpr friend auto as(Self *self) -> Derived *;

    /// `magic` is the derived class's own `gktl::magic<Derived>()`, passed in
    /// because only the derived constructor knows what it is being built as,
    /// and the canary has to hold that from the start. This is the shape
    /// `VmAreaOrMapping` already uses, for the same reason.
    ///
    /// `lock` is the mutex lock() will answer (see there); it must outlive
    /// this object, which it does by being the derived class's to keep.
    VmObject(u32 magic, Type type, Flags vmof, sync::Mutex &lock);

    virtual ~VmObject() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(VmObject);

    /// Under the object's lock, because the mappings of one object can belong
    /// to different address spaces: each of those holds only its own aspace
    /// lock, and two of them adding a mapping of this object at once would
    /// otherwise write this list concurrently.
    TA_REQUIRES(m_lock)
    FORCE_INLINE
    auto add_mapping(VmMapping &mapping) -> void {
        m_mappings.push_back(mapping);
    }

    TA_REQUIRES(m_lock)
    FORCE_INLINE
    auto remove_mapping(VmMapping &mapping) -> void {
        auto _ = m_mappings.erase(m_mappings.iterator_to(mapping));
    }

    gktl::Canary m_canary;
    Type const m_type;
    Flags m_flags;

    sync::Mutex &m_lock;
    VmMapping::List m_mappings TA_GUARDED(m_lock);

    ktl::Name<MAX_NAME_LEN> m_name;
    std::atomic<u64> m_user_id{0};

    ustl::collections::intrusive::ListMemberHook<> m_children_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_children_hook, ChildrenOptions);
    ustl::collections::intrusive::List<Self, ChildrenOptions> m_children;
};

using VmoFlags = VmObject::Flags;

template <typename Derived>
FORCE_INLINE
constexpr auto as(VmObject *vmo) -> Derived * {
    using ustl::traits::IsSameV;

    auto const type = vmo->m_type;
    if constexpr (IsSameV<Derived, VmObjectPaged>) {
        if (type == VmObject::Type::Paged) {
            return static_cast<Derived *>(vmo);
        }
    } else if constexpr (IsSameV<Derived, VmObjectPhysical>) {
        if (type == VmObject::Type::Physical) {
            return static_cast<Derived *>(vmo);
        }
    }
    return nullptr;
}

template <typename Derived>
FORCE_INLINE
constexpr auto as(ustl::Arc<VmObject> vmo) -> ustl::Arc<Derived> {
    return ustl::Arc<Derived>::adopt(as<Derived>(vmo.as_ptr_mut()));
}

class VmObject::Builder {
    using Self = Builder;

  public:
    auto set_type(Type type) -> Self & {
        m_type = type;
        return *this;
    }

    auto build(ustl::Arc<VmObject> &out) -> Status;

  private:
    Type m_type;
    usize m_offset;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_OBJECT_HPP