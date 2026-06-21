/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_ASPACE_HPP
#define OURS_MEM_VM_ASPACE_HPP 1

#if !__has_include(<ours/arch/vm_aspace.hpp>)
#   error "The header <ours/arch/vm_aspace.hpp> is required by [protocol.module.mem]"
#endif
#include <ours/arch/vm_aspace.hpp>

#include <ours/mem/vm_fault.hpp>
#include <ours/mem/vm-aspace-flags.hpp>

#include <ours/mem/concepts/arch-vm-aspace.hpp>

#include <ours/init.hpp>

#include <ustl/rc.hpp>
#include <ustl/sync/mutex.hpp>
#include <ustl/sync/atomic.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/collections/intrusive/list.hpp>

#include <gktl/range.hpp>
#include <gktl/canary.hpp>

#include <ktl/result.hpp>

namespace ours::mem {
    static_assert(concepts::ArchVmAspace<ArchVmAspace>);

    class VmAspace: public ustl::RefCounter<VmAspace> {
        using Self = VmAspace;
        using Base = ustl::RefCounter<VmAspace>;
      public:
        using Flags = VmasFlags;

        struct Usage {
            usize mapped_pages;
            usize private_pages;
            usize shared_pages;
        };

        static auto clone(Flags, ustl::Rc<VmAspace> *) -> Status;

        static auto create(Flags, char const *, ustl::Rc<VmAspace> *out) -> Status;

        static auto create(VirtAddr, usize, Flags, char const *, ustl::Rc<VmAspace>*) -> Status;

        /// Perform the initialization of kernel address space.
        /// Just once called at boot-time.
        static auto init_kernel_aspace() -> void;

        /// Synchronize changes from kernel's address space into all user's.
        static auto sync_kernel_aspace() -> void;

        static auto switch_aspace(Self *, Self *) -> void;

        static auto kernel_aspace() -> ustl::Rc<VmAspace> {
            return ustl::make_rc<VmAspace>(g_kernel_aspace);
        }

        auto get_memory_usage() -> ktl::Result<Usage>;

        auto page_fault(VirtAddr va, VmfCause cause) -> Status;

        auto soft_fault(VirtAddr va, VmfCause cause) -> Status;

        auto accessed_fault(VirtAddr va) -> Status;

        FORCE_INLINE
        auto get_lock() -> sync::SpinLock & { return m_lock; }

        FORCE_INLINE
        auto is_user() const -> bool { return !!(m_flags & Flags::User); }

        FORCE_INLINE
        auto arch_aspace() -> ArchVmAspace &  { return m_arch; }

        auto root_vma() -> ustl::Rc<VmArea>;

        VmAspace(VirtAddr, usize, Flags, char const *);
        ~VmAspace();
      private:
        friend class VmArea;
        friend class VmMapping;

        struct AlsrConfig {
            u8 entropy_bits;
            u8 compact_entropy_bits;
            // We record the PRNG seed to enable reproducible debugging.
            u8 seed[2];
        };

        auto init() -> Status;

        auto page_fault_internal(VirtAddr va, VmfCause cause, usize extra_pages) -> Status;

        gktl::Canary m_canary;
        Flags m_flags;
        VirtAddr m_base;
        VirtAddr m_size;
        sync::SpinLock m_lock;

        /// Architecture specific context.
        ArchVmAspace m_arch;

        /// How many users hold this.
        ustl::sync::AtomicU32 m_refcnt;
        ustl::sync::AtomicU32 m_num_users;

        /// The only root region.
        ustl::Rc<VmArea> m_root_vma;
        ustl::Rc<VmMapping> m_last_fault;

        AlsrConfig m_alsr_config;

        ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOptions);
        USTL_DECLARE_LIST(Self, List, ManagedOptions);

        /// The list which strungs all existing VmAspace.
        static Self::List g_all_aspace_list;
        static sync::SpinLock g_all_aspace_list_lock;

        static VmAspace *g_kernel_aspace;
        static sync::SpinLock g_kernel_aspace_lock;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_ASPACE_HPP