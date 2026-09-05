/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_OBJECT_HPP
#define OURS_MM_VM_OBJECT_HPP

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ktl/name.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/status.hpp>
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

    /// Read the |size| of bytes from |offset| into |out|
    virtual auto read(void *outbuf, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

    /// Read the |size| of bytes from |offset| into |out|
    virtual auto write(void *inbuf, VirtAddr offset, usize size) -> Status {
        return Status::Unsupported;
    }

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

    FORCE_INLINE
    auto add_mapping(VmMapping &mapping) -> void {
        m_mappings.push_back(mapping);
    }

    FORCE_INLINE
    auto remove_mapping(VmMapping &mapping) -> void {
        (void)m_mappings.erase(m_mappings.iterator_to(mapping));
    }

    gktl::Canary m_canary;
    Type const m_type;
    Flags m_flags;
    VmMapping::List m_mappings;

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