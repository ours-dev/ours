/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_RESOURCE_DISPATCHER_HPP
#define OURS_OBJECT_RESOURCE_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
/// Rights a freshly created resource handle carries: a resource is not
/// waited on.
constexpr auto RIGHTS_DEFAULT_RESOURCE =
    (Rights::Basic & ~Rights::Wait) | Rights::Write | Rights::GetProperty;

using ResourceDispatcherBase = SoloDispatcher<ResourceDispatcher, RIGHTS_DEFAULT_RESOURCE>;

class ResourceDispatcher: public ResourceDispatcherBase {
    using Base = ResourceDispatcherBase;
    using Self = ResourceDispatcher;

    // A member hook, like ThreadDispatcher's, rather than a ListBaseHook:
    // the List alias below is named while ResourceDispatcher is still
    // incomplete, and base-hook value traits cannot cast between the hook
    // and an incomplete owner. Declared up here because Storage needs List.
    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Resource);

    using Kind = RsrcKind;
    using Flag = RsrcFlag;
    using Name = ktl::Name<MAX_NAME_LEN>;
    USTL_DECLARE_LIST(Self, List, ManagedOption);

    struct Storage {
        Self::List resources;
    };

    /// Mints a resource over [base, base + size) of |kind|.
    ///
    /// Only what M1 needs: the System kind, one slot at a time -- size 1 at a
    /// base below RSRC_SYSTEM_COUNT (InvArgs otherwise). Every other kind is
    /// NotSupported until something needs it; MMIO, IRQ and I/O-port ranges
    /// bring overlap checks against the ranges already minted, kept in
    /// Storage, which is why |storage| is accepted and not yet used.
    static auto create(KernelHandle<Self> *out, Rights &rights, Kind kind, PhysAddr base,
                       PhysAddr size, Flag flags, Name const &name, Storage *storage = nullptr)
        -> Status;

    [[nodiscard]]
    auto kind() const -> Kind {
        return m_kind;
    }

    [[nodiscard]]
    auto base() const -> PhysAddr {
        return m_base;
    }

    [[nodiscard]]
    auto size() const -> PhysAddr {
        return m_size;
    }

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    ResourceDispatcher(Kind kind, PhysAddr base, PhysAddr size, Flag flags, Name const &);
    ~ResourceDispatcher() = default;

    static constexpr auto MAGIC = gktl::magic("ResourceDispatcher");

    gktl::Canary m_canary;
    Name m_name;
    Kind m_kind;
    Flag m_flags;
    PhysAddr m_base;
    PhysAddr m_size;
    Self::List *m_rsrclist;

    static inline Storage SYSTEM_RESOURCES;
};
/// Whether |handle|, in the calling process's table, names a resource of
/// |kind| whose range covers all of [base, base + size): Ok, or why not.
///
///   - BadHandle when |handle| names nothing;
///   - BadType when it names something other than a resource, or a resource
///     of another kind;
///   - OutOfRange when the resource does not cover the range.
///
/// No right is required of the handle: holding it is the capability. This is
/// how a system call asks "may the caller do X" for the X a System slot names
/// -- vmo_replace_as_executable asks it for VMEX.
[[nodiscard]]
auto validate_ranged_resource(ours::Uandle handle, RsrcKind kind, u64 base, u64 size) -> Status;
} // namespace ours::object

#endif // OURS_OBJECT_RESOURCE_DISPATCHER_HPP
