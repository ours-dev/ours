/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_AREA_OR_MAPPING_HPP
#define OURS_MM_VM_AREA_OR_MAPPING_HPP 1

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>
#include <ustl/collections/intrusive/any_hook.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>
#include <ustl/result.hpp>
#include <ustl/util/move.hpp>

namespace ours::mm {
enum class VmMapOption {
    // For semenatics, no any effects.
    None,
    Commit = BIT(0),
    Pinned = BIT(1),
    Fixed = BIT(2),
    FixedNoReplace = BIT(3),
};
USTL_ENABLE_ENUM_BITMASK(VmMapOption);

enum class VmaFlags : u32 {
    // For semantics, no any actual effects.
    None,

    // Only serve for VmMapping
    Write = 0_bit,
    Read = 1_bit,
    Exec = 2_bit,
    Share = 3_bit,
    PermMask = Write | Read | Exec | Share,

    // States
    Active = 0x10000,
    Pinned = 0x20000,

    // Features.
    Anonymous = 0x40000,
    Mergeable = 0x80000,

    // Types
    Area = 0x0000'0000, // For semantics, no any actual effects.
    Mapping = 0x1000'0000,
};
USTL_ENABLE_ENUM_BITMASK(VmaFlags);

FORCE_INLINE constexpr auto extract_permissions(VmaFlags vmaflags) -> MmuFlags {
    MmuFlags mmuf{};
    if (!!(vmaflags & VmaFlags::Write)) {
        mmuf |= MmuFlags::Writable;
    }
    if (!!(vmaflags & VmaFlags::Read)) {
        mmuf |= MmuFlags::Readable;
    }
    if (!!(vmaflags & VmaFlags::Exec)) {
        mmuf |= MmuFlags::Executable;
    }

    return mmuf;
}

class VmAreaOrMapping: public ustl::RefCounted<VmAreaOrMapping, ustl::sync::ThreadSafePolicy> {
    using Self = VmAreaOrMapping;
    using Base = ustl::RefCounted<Self, ustl::sync::ThreadSafePolicy>;

    // Base::destroy() calls ~VmAreaOrMapping() through the base class on the
    // final release(); without this friend, that call is ill-formed since
    // the destructor below is protected.
    friend Base;

  public:
    class Builder;

    static constexpr std::uintptr_t TYPE_TAG = 1;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }

    using Flags = VmaFlags;

    FORCE_INLINE
    auto aspace() -> ustl::Arc<VmAspace> & {
        return m_aspace;
    }

    FORCE_INLINE
    auto base() const -> VirtAddr {
        return m_base;
    }

    FORCE_INLINE
    auto size() const -> VirtAddr {
        return m_size;
    }

    FORCE_INLINE
    auto is_active() const -> bool {
        return !!(m_vmaf & VmaFlags::Active);
    }

    FORCE_INLINE
    auto is_mapping() const -> bool {
        return !!(m_vmaf & VmaFlags::Mapping);
    }

    FORCE_INLINE
    auto get_lock() -> sync::SpinLock & {
        return m_aspace->get_lock();
    }

    FORCE_INLINE
    auto check_range(VirtAddr base, usize size) const -> bool {
        return base >= m_base && size <= m_size && (base - m_base) <= m_size - size;
    }

    /// This for the collections to compare two VMA.
    FORCE_INLINE
    friend auto operator<(Self const &x, Self const &y) noexcept -> bool {
        return x.m_base < y.m_base;
    }

    struct BaseOrder final {
        FORCE_INLINE
        auto operator()(Self const &x, Self const &y) const noexcept -> bool {
            return x.m_base < y.m_base;
        }
    };

  protected:
    constexpr static auto const kAreaMagic = gktl::magic("VmArea");

    constexpr static auto const kMappingMagic = gktl::magic("VmMapping");

    VmAreaOrMapping(u32 magic, VirtAddr base, usize size, VmaFlags vmaf, VmArea *, VmAspace *,
                    char const *);
    ~VmAreaOrMapping() = default;

    /// Check whether a given `|mmuflags|` is valid. True if valid, otherwise invalid.
    auto validate_mmuflags(MmuFlags mmuf) const -> bool;

    /// Check whether a given `|vmaflags|` is valid. True if valid, otherwise invalid.
    FORCE_INLINE
    auto validate_vmaflags(VmaFlags vmaf) const -> bool {
        auto perm_allowed = (m_vmaf & VmaFlags::PermMask);
        if (((vmaf & VmaFlags::PermMask) | perm_allowed) != perm_allowed) {
            return false;
        }

        return true;
    }

    FORCE_INLINE
    auto mark_active() -> void {
        m_vmaf |= VmaFlags::Active;
    }

    virtual auto activate() -> Status;

    /// This routine by default just remove itself.
    virtual auto destroy() -> Status;

    friend class VmObject;
    friend class VmAspace;
    friend class VmaSet;
    friend class VmArea;

    gktl::Canary m_canary;
    char const *m_name;
    VirtAddr m_base;
    VirtAddr m_size;
    VmaFlags m_vmaf;
    VmArea *m_parent;
    ustl::Arc<VmAspace> m_aspace;
    ustl::collections::intrusive::SetMemberHook<> m_children_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_children_hook, ManagedOptions);
};
USTL_DECLARE_MULTISET(VmAreaOrMapping, VmaSetBase,
                      ustl::collections::intrusive::Compare<VmAreaOrMapping::BaseOrder>,
                      VmAreaOrMapping::ManagedOptions);

class VmAreaOrMapping::Builder {
    using Self = VmAreaOrMapping::Builder;

  public:
    Builder(ustl::Arc<VmAspace> *aspace, VmaFlags vmaf, usize size);
    ~Builder() = default;

    auto build(ustl::Arc<VmAreaOrMapping> *out) -> Status;

    auto set_base(VirtAddr base) -> Self & {
        m_base = base;
        return *this;
    }

    auto set_parent(VmArea *parent) -> Self & {
        m_parent = parent;
        return *this;
    }

    auto set_aspace(ustl::Arc<VmAspace> *aspace) -> Self & {
        m_aspace = aspace;
        return *this;
    }

    auto set_vmo(ustl::Arc<VmObject> vmo) -> Self &;

    auto set_vmo_offset(usize vmo_ofs) -> Self & {
        m_vmo_ofs = vmo_ofs;
        return *this;
    }

    auto set_name(char const *nm) -> Self & {
        m_name = nm;
        return *this;
    }

    auto set_option(VmMapOption option) -> Self & {
        m_option = option;
        return *this;
    }

    auto set_mmuf(MmuFlags mmuf) -> Self & {
        m_mmuf = mmuf;
        return *this;
    }

  private:
    VirtAddr m_base;
    usize m_size;
    VmaFlags m_vmaf;
    MmuFlags m_mmuf;
    VmMapOption m_option;
    VmArea *m_parent;
    ustl::Arc<VmAspace> *m_aspace;
    char const *m_name;
    usize m_vmo_ofs;
    ustl::Arc<VmObject> m_vmo;
};

class VmaSet: public VmaSetBase {
    using Self = VmaSet;
    using Base = VmaSetBase;

    using Base::erase;
    using Base::insert;

  public:
    class Enumerator;
    typedef Base::Element Element;
    typedef Base::PtrMut PtrMut;
    typedef Base::Ptr Ptr;
    typedef Base::RefMut RefMut;
    typedef Base::Ref Ref;
    typedef Base::IterMut IterMut;
    typedef Base::Iter Iter;

    /// Determine that in this set if a region contains the range [base, base + size).
    ///
    /// Return true if contains, otherwise false.
    auto has_range(VirtAddr base, usize size) const -> bool;

    auto find_spot(usize size, usize align, VirtAddr lower_bound, VirtAddr upper_bound) const
        -> ustl::Result<VirtAddr, Status>;

    // Now there are so many restrictions that we must manually manage those reference counters
    // from instances of VmAom.

    FORCE_INLINE
    auto insert(Iter hint, RefMut value) -> IterMut {
        value.retain();
        return Base::insert(hint, value);
    }

    FORCE_INLINE
    auto insert(RefMut value) -> IterMut {
        value.retain();
        return Base::insert(value);
    }

    FORCE_INLINE
    auto erase(Iter pos) -> IterMut {
        pos.unconst()->release();
        return Base::erase(pos);
    }

    FORCE_INLINE
    auto erase(Iter first, Iter last) -> IterMut {
        return Base::erase_and_dispose(first, last,
                                       [](PtrMut value) noexcept { value->release(); });
    }

    /// Find the first existent VMA whose'end < addr.
    FORCE_INLINE
    auto lower_bound(VirtAddr addr) -> IterMut {
        return Base::lower_bound(addr, [](Ref x, VirtAddr y) noexcept { return x.m_base < y; });
    }

    FORCE_INLINE
    auto lower_bound(VirtAddr addr) const -> Iter {
        return Base::lower_bound(addr, [](Ref x, VirtAddr y) noexcept { return x.m_base < y; });
    }

    /// Find the first existent VMA whose'base > addr.
    FORCE_INLINE
    auto upper_bound(VirtAddr addr) -> IterMut {
        return Base::upper_bound(addr, [](VirtAddr y, Ref x) noexcept { return y <= x.m_base; });
    }

    FORCE_INLINE constexpr auto upper_bound(VirtAddr addr) const -> Iter {
        return Base::upper_bound(addr, [](VirtAddr y, Ref x) noexcept { return y <= x.m_base; });
    }
};

class VmaSet::Enumerator {
  public:
    Enumerator(VmaSet &, VirtAddr min, VirtAddr max);
    Enumerator(VmArea &, VirtAddr min, VirtAddr max);
    ~Enumerator() = default;

  private:
    IterMut iter_;
    VmaSet *vmaset_;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_AREA_OR_MAPPING_HPP
