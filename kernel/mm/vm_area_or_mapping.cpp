#include <logz4/log.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
auto VmAreaOrMapping::Builder::set_vmo(ustl::Arc<VmObject> vmo) -> Self & {
    m_vmo = ustl::move(vmo);
    return *this;
}

VmAreaOrMapping::VmAreaOrMapping(u32 magic, VirtAddr base, usize size, VmaFlags vmaf,
                                 VmArea *parent, VmAspace *aspace, char const *name)
    : m_canary(magic)
    , m_base(base)
    , m_size(size)
    , m_vmaf(vmaf)
    , m_parent(parent)
    , m_aspace(aspace)
    , m_name(name)
    , m_children_hook() {}

auto VmAreaOrMapping::validate_mmuflags(MmuFlags mmuf) const -> bool {
    VmaFlags vmaf{};
    if (!!(mmuf & MmuFlags::Readable)) {
        vmaf |= VmaFlags::Read;
    }
    if (!!(mmuf & MmuFlags::Writable)) {
        vmaf |= VmaFlags::Write;
    }
    if (!!(mmuf & MmuFlags::Executable)) {
        vmaf |= VmaFlags::Exec;
    }

    auto const perm_allowed = (m_vmaf & VmaFlags::PermMask);
    if (((vmaf & VmaFlags::PermMask) | perm_allowed) != perm_allowed) {
        return false;
    }

    // Except the requirements above, a qualifying MMU flag has at least one
    // performing permission yet.
    return !!(mmuf & MmuFlags::PermMask);
}

auto VmAreaOrMapping::activate() -> Status {
    mark_active();
    m_parent->m_subvmas.insert(*this);
    return Status::Ok;
}

auto VmAreaOrMapping::destroy() -> Status {
    m_vmaf &= ~VmaFlags::Active;
    if (m_parent) {
        auto to_erase = m_parent->m_subvmas.iterator_to(*this);
        m_parent->m_subvmas.erase(to_erase);
        m_parent = nullptr;
    }
    return Status::Ok;
}

auto VmaSet::has_range(VirtAddr base, usize size) const -> bool {
    auto iter = upper_bound(base + size);
    if (iter != end()) {
        if (iter->m_base <= base + size - 1) {
            return true;
        }
    }

    --iter;
    if (iter != end()) {
        if (iter->m_base + iter->m_size - 1 >= base) {
            return true;
        }
    }

    return false;
}

auto VmaSet::find_spot(usize size, usize align, VirtAddr low_limit, VirtAddr high_limit) const
    -> ustl::Result<VirtAddr, Status> {
    if (!size) {
        return ustl::err(Status::InvalidArguments);
    }
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }

    // The core logic is to find a gap between two existing areas, if the gap has enough size
    // then we return the base address of it. Range [low_limit, high_limit) must be a available
    // address interval to avoid doing additional bound check.
    auto requested_size = ustl::mm::align_up(size, align);
    VirtAddr addr = ustl::mm::align_up(low_limit, PAGE_SIZE);
    for (auto i = upper_bound(addr), last = end(); i != last; ++i) {
        if (addr > high_limit) {
            return ustl::err(Status::NotFound);
        }
        if (i->m_base >= addr + requested_size) {
            return ustl::ok(addr);
        }

        addr = ustl::mm::align_up(i->m_base + i->m_size, align);
    }

    if (addr + requested_size <= high_limit) {
        return ustl::ok(addr);
    }
    return ustl::err(Status::NotFound);
}

VmAreaOrMapping::Builder::Builder(ustl::Arc<VmAspace> *aspace, VmaFlags vmaf, usize size)
    : m_aspace(aspace), m_vmaf(vmaf), m_size(size) {}

auto VmAreaOrMapping::Builder::build(ustl::Arc<VmAreaOrMapping> *out) -> Status {
    if (!out) {
        return Status::InvalidArguments;
    }

    if (m_vmo) {
        ustl::Arc<VmMapping> mapping;
        auto status = VmMapping::create(m_parent, m_base, m_size, m_vmaf, ustl::move(m_vmo),
                                        m_vmo_ofs, m_mmuf, m_name, &mapping);
        if (Status::Ok != status) {
            return status;
        }
        *out = ustl::adopt<VmAreaOrMapping>(ustl::move(mapping));
    } else {
        ustl::Arc<VmArea> vma;
        auto status =
            VmArea::create(m_base, m_size, m_vmaf, m_parent, m_aspace->as_ptr_mut(), m_name, &vma);
        if (Status::Ok != status) {
            return status;
        }
        *out = ustl::adopt<VmAreaOrMapping>(ustl::move(vma));
    }

    return Status::Ok;
}

} // namespace ours::mm
