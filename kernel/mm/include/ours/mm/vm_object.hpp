/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_OBJECT_HPP
#define OURS_MM_VM_OBJECT_HPP

#include <ours/mm/types.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>

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
        Pinned = BIT(0),

        /// Never pre-commit pages, only when the page access fault occurs.
        Lazy = BIT(1),

        ///
        Resizable = BIT(2),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    enum class CommitOptions {
        /// No any effect, just for semantics.
        None,

        /// The pages mapped will not be swaped out.
        Pin = BIT(0),

        /// Map imediately but wait a page access fault come.
        Write = BIT(1),
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

    /// Remove all pages in range [pgoff, pgoff + n) to |pages|.
    ///
    /// Return Status::Ok if successfully taken.
    virtual auto take_pages(VirtAddr offset, usize size, VmPage::List *pages) -> Status {
        return Status::Unsupported;
    }

    enum class SupplyOption : u8 {
        /// Overwrite those pages which already exists.
        TransferData = BIT(0),

        /// If the range already exists, release the pages supplied.
        AutoRelease = BIT(1),

        Default = AutoRelease,
    };
    /// Using enum bitflags for the extension in future.
    USTL_ENABLE_INNER_ENUM_BITMASK(SupplyOption);

    /// Supplies this VMO with |pages| for the page range [pgoff, pgoff + n).
    /// Typically it should be invoked by PageProvider.
    virtual auto supply_pages(VirtAddr offset, usize size, VmPage::List *pages,
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

    /// Copies |size| bytes starting at byte |offset| of this object straight
    /// into the user buffer |out|, with no kernel buffer in between.
    ///
    /// The copy runs under the object's lock, so a page fault on the user side
    /// is captured rather than resolved in place, and the lock is dropped to
    /// resolve it; see VmObjectPaged::copy_page_by_page(). InvArgs when some page of
    /// the user buffer cannot be made present, OutOfRange as read() has it.
    /// The user bytes before a failure may have been written.
    ///
    /// Unsupported by default, which is where a physical VMO stays: Zircon's
    /// VmObject::ReadUser() answers ZX_ERR_NOT_SUPPORTED for one too.
    virtual auto read_user(user_copy::UserOutPtr<u8> out, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

    /// Copies |size| bytes from the user buffer |in| straight into this object,
    /// starting at byte |offset|. The same rules as read_user().
    virtual auto write_user(user_copy::UserInPtr<u8> in, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

    /// The object's size in bytes, which is the bound read(), write() and
    /// commit_range() check a range against. Zircon: VmObject::size_locked().
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
        m_name.set(name);
    }

    FORCE_INLINE
    auto type() const -> Type {
        return m_type;
    }

    /// The object's lock: over its pages (a paged VMO's page tree) and over
    /// the list of mappings of it. One lock for both, as Zircon has one, and
    /// for the reason Zircon does: an operation that changes pages -- a
    /// decommit, say -- has to reach every mapping of them while no page and
    /// no mapping can change under it.
    ///
    /// Taken inside an address space's lock, never the other way round:
    /// VmMapping::page_fault(), map_paged(), activate() and destroy() all run
    /// with the aspace lock held and take this one inside it, and
    /// VmObjectPaged::copy_page_by_page() lets go of this one before it
    /// resolves a user fault. Zircon's VmMapping::PageFault() takes
    /// `paged->lock()` with the aspace lock already held.
    ///
    /// It lives here and not in VmObjectPaged, where it began, because the
    /// mapping list does: a physical VMO has mappings too.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto lock() -> sync::Mutex & {
        return m_lock;
    }

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
    VmObject(u32 magic, Type type, Flags vmof);

    virtual ~VmObject() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(VmObject);

    /// Under the object's lock, because the mappings of one object can belong
    /// to different address spaces: each of those holds only its own aspace
    /// lock, and two of them adding a mapping of this object at once would
    /// otherwise write this list concurrently. Zircon:
    /// VmObject::AddMappingLocked(), TA_REQ(lock()).
    TA_REQUIRES(m_lock)
    FORCE_INLINE
    auto add_mapping(VmMapping &mapping) -> void {
        m_mappings.push_back(mapping);
    }

    TA_REQUIRES(m_lock)
    FORCE_INLINE
    auto remove_mapping(VmMapping &mapping) -> void {
        (void)m_mappings.erase(m_mappings.iterator_to(mapping));
    }

    gktl::Canary m_canary;
    Type const m_type;
    Flags m_flags;

    sync::Mutex m_lock;
    VmMapping::List m_mappings TA_GUARDED(m_lock);

    constexpr static auto MAX_NAME_SIZE = 32;
    ktl::Name<MAX_NAME_SIZE> m_name;

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