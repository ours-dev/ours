/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_PAGE_HPP
#define OURS_MEM_VM_PAGE_HPP 1

#include <ours/mem/types.hpp>
#include <ours/mem/pm-frame.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::mem {
    /// When a or a group of frame was used in VMM, it shows the following layout.
    class alignas(PmFrame::kAlignment) VmPage: public PageFrame {
        using Self = VmPage;
      public:
        CXX11_CONSTEXPR
        static auto kRole = Role::Vmm;

        auto set_object(VmObject *object) -> void { m_vmo = object; }

        auto get_object() -> void * { return m_vmo; }

        auto set_pgoff(PgOff pgoff) -> void { m_vmo_index = pgoff; }

        auto get_pgoff() const -> PgOff { return m_vmo_index; }

        auto increase_mappings(usize num = 1) -> void { m_num_mappings += num; }
        auto decrease_mappings(usize num = 1) -> void { m_num_mappings -= num; }
        auto num_mappings() const -> usize { return m_num_mappings; }
      private:
        VmObject *m_vmo; // For reverse mapping.
        ustl::sync::AtomicU32 m_vmo_index;    // Index in VMO's page map
        ustl::sync::AtomicU16 m_num_mappings; // 65536 mappins 
        ustl::sync::AtomicU16 m_num_users;    // 65536 users.
        ustl::sync::AtomicU8 pined_count;

        ustl::collections::intrusive::ListMemberHook<> m_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_hook, ManagedOption);
      public:
        USTL_DECLARE_LIST(VmPage, List, ManagedOption, ustl::collections::intrusive::ConstantTimeSize<false>);
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_PAGE_HPP