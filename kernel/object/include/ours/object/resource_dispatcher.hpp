/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_RESOURCE_DISPATCHER_HPP
#define OURS_OBJECT_RESOURCE_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>
#include <ours/object_info.hpp>
#include <ours/resource.hpp>
#include <ours/syscall.hpp>

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

    /// Mints a resource over [base, base + size) of |kind|:
    ///
    ///   - System: one slot at a time, size 1 at a base below
    ///     RSRC_SYSTEM_COUNT;
    ///   - Irq, IoPort and MmIo: base 0 and size 0 for the kind's root, which
    ///     covers the kind's whole space (interrupt lines are numbered by a
    ///     u32, I/O ports by a u16, physical addresses by as many bits as the
    ///     CPU puts on its bus), or a range of at least one number inside
    ///     that space.
    ///
    /// InvArgs for a range the kind does not allow and for a number that
    /// names no kind. No range is checked against the kernel's own ports,
    /// memory or devices here: holding a resource over one grants nothing,
    /// since every use goes through validate_ranged_resource(), which refuses
    /// them.
    ///
    /// Exclusive ranges will need overlap checks against the ranges already
    /// minted, kept in Storage, which is why |storage| is accepted and not
    /// yet used.
    static auto create(KernelHandle<Self> *out, Rights &rights, Kind kind, u64 base, u64 size,
                       Flag flags, Name const &name, Storage *storage = nullptr) -> Status;

    [[nodiscard]]
    auto kind() const -> Kind {
        return m_kind;
    }

    /// The range is counted in the kind's own numbers: a physical address
    /// only for MmIo, an interrupt line, an I/O port or a System slot for the
    /// others. So it is a plain u64, as validate_ranged_resource() and the
    /// info record take it, and not a PhysAddr.
    [[nodiscard]]
    auto base() const -> u64 {
        return m_base;
    }

    [[nodiscard]]
    auto size() const -> u64 {
        return m_size;
    }

    /// Whether this is the root of a ranged kind, base 0 and size 0, which
    /// covers the kind's whole space rather than an empty range.
    [[nodiscard]]
    auto is_root() const -> bool;

    /// What object_get_info answers for InfoTopic::Resource.
    [[nodiscard]]
    auto get_info() const -> InfoResource;

  private:
    ResourceDispatcher(Kind kind, u64 base, u64 size, Flag flags, Name const &);
    ~ResourceDispatcher() = default;

    Name m_name;
    Kind m_kind;
    Flag m_flags;
    u64 m_base;
    u64 m_size;
    // Set and not read yet. Nothing is a friend of this class any more, so
    // the compiler can see that, and would refuse it as unused.
    [[maybe_unused]]
    Self::List *m_rsrclist;

    static inline Storage SYSTEM_RESOURCES;
};
/// Whether |resource| is of |kind| and grants all of [base, base + size): Ok,
/// or why not.
///
///   - BadType when it is a resource of another kind;
///   - OutOfRange when its range does not cover the request; a root covers
///     its kind's whole space;
///   - AccessDenied when the request touches a port the kernel drives itself
///     (platform::kernel_ioports()), or physical memory or a device it owns
///     (platform::touches_kernel_physical()), which no resource grants, not
///     even the root.
[[nodiscard]]
auto validate_ranged_resource(ResourceDispatcher const &resource, RsrcKind kind, u64 base, u64 size)
    -> Status;

/// The same for the resource |handle| names in the calling process's table,
/// with two answers more: BadHandle when |handle| names nothing, and BadType
/// when it names something other than a resource.
///
/// No right is required of the handle: holding it is the capability. This is
/// how a system call asks "may the caller do X" for the X a System slot names
/// -- vmo_replace_as_executable asks it for VMEX.
[[nodiscard]]
auto validate_ranged_resource(ours::Uandle handle, RsrcKind kind, u64 base, u64 size) -> Status;

/// Whether |resource| is of |kind| and is its root, the resource made with base
/// 0 and size 0 (is_root()): Ok, or why not.
///
///   - BadType when it is a resource of another kind;
///   - AccessDenied when it is not the root: a range, even one that covers
///     the kind's whole space, or a kind that has no space to cover.
///
/// For a call that is the root's alone, such as naming an interrupt source by
/// the firmware's number, and so cannot be granted by a range.
[[nodiscard]]
auto validate_root_resource(ResourceDispatcher const &resource, RsrcKind kind) -> Status;

/// The same for the resource |handle| names in the calling process's table,
/// with two answers more: BadHandle when |handle| names nothing, and BadType
/// when it names something other than a resource. No right is required of the
/// handle, as for validate_ranged_resource().
[[nodiscard]]
auto validate_root_resource(ours::Uandle handle, RsrcKind kind) -> Status;
} // namespace ours::object

#endif // OURS_OBJECT_RESOURCE_DISPATCHER_HPP
