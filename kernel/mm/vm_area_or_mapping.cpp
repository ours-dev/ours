#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_area_or_mapping.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>

namespace ours::mm {
auto VmAreaOrMapping::Builder::set_vmo(ustl::Arc<VmObject> vmo) -> Self & {
    m_vmo = ustl::move(vmo);
    return *this;
}

VmAreaOrMapping::VmAreaOrMapping(u32 magic, VirtAddr base, usize size, VmaFlags vmaf,
                                 VmArea *parent, VmAspace *aspace, char const *name)
    : m_canary(magic)
    , m_name(name)
    , m_base(base)
    , m_size(size)
    , m_vmaf(vmaf)
    , m_parent(parent)
    , m_aspace(aspace) {}

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
    return m_parent->m_subvmas.insert(*this);
}

auto VmAreaOrMapping::destroy() -> Status {
    m_vmaf &= ~VmaFlags::Active;
    if (m_parent) {
        m_parent->m_subvmas.erase(*this);
        m_parent = nullptr;
    }
    return Status::Ok;
}

VmaSet::~VmaSet() {
    // Reaching here with areas still present means someone tore down the
    // owning VmArea without unmapping its children first; draining keeps
    // that from silently leaking the references the set took, and the
    // assertion says so in a debug build rather than hiding it.
    DEBUG_ASSERT(m_tree.empty(), "destroying a VmaSet that still holds areas");
    clear();
}

auto VmaSet::clear() -> void {
    // Drop the set's reference on each area before the pointers go away --
    // this is the only place the "leaves the tree before it is destroyed"
    // invariant could otherwise be broken.
    for (auto *area: m_tree) {
        area->release();
    }
    m_tree.clear();
}

auto VmaSet::insert(VmAreaOrMapping &value) -> Status {
    auto result = m_tree.insert(&value);
    if (result.is_err()) {
        return Status::OutOfMem;
    }
    if (!result.unwrap().second) {
        // An area with this base is already here; nothing was inserted, so
        // no reference is taken.
        return Status::AlreadyExists;
    }
    value.retain();
    return Status::Ok;
}

auto VmaSet::erase(VmAreaOrMapping &value) -> void {
    if (m_tree.erase(value.base()) != 0) {
        value.release();
    }
}

auto VmaSet::erase(IterMut pos) -> IterMut {
    if (pos == m_tree.end()) {
        return m_tree.end();
    }

    auto *const value = *pos;
    auto const next = m_tree.erase(pos);
    value->release();
    return next;
}

auto VmaSet::seek(VirtAddr addr) -> IterMut {
    auto const at_or_after = m_tree.lower_bound(addr);
    auto iter = at_or_after.is_some() ? at_or_after.value() : m_tree.end();

    if (iter != m_tree.begin()) {
        auto previous = iter;
        --previous;
        if ((*previous)->end() > addr) {
            return previous;
        }
    }

    return iter;
}

auto VmaSet::seek(VirtAddr addr) const -> Iter {
    auto const at_or_after = m_tree.lower_bound(addr);
    auto iter = at_or_after.is_some() ? at_or_after.value() : m_tree.end();

    if (iter != m_tree.begin()) {
        auto previous = iter;
        --previous;
        if ((*previous)->end() > addr) {
            return previous;
        }
    }

    return iter;
}

VmaSet::Enumerator::Enumerator(VmaSet &set, VirtAddr min, VirtAddr max)
    : m_set(&set), m_iter(set.seek(min)), m_max(max) {}

VmaSet::Enumerator::Enumerator(VmArea &area, VirtAddr min, VirtAddr max)
    : Enumerator(area.m_subvmas, min, max) {}

auto VmaSet::Enumerator::next() -> VmAreaOrMapping * {
    if (m_iter == m_set->end()) {
        return nullptr;
    }

    auto *const area = *m_iter;
    if (area->base() >= m_max) {
        // Ordered by base, so nothing further can overlap either. Park on end()
        // so a caller that keeps asking keeps getting nullptr.
        m_iter = m_set->end();
        return nullptr;
    }

    // Past the area before handing it over: the caller is allowed to drop its
    // reference to what it gets back.
    ++m_iter;
    return area;
}

auto VmaSet::has_range(VirtAddr base, usize size) const -> bool {
    // seek() already answers "the first area that can reach into base", the
    // straddler included, so all that is left is whether it starts before the
    // window ends.
    auto const iter = seek(base);
    if (iter != m_tree.end() && (*iter)->base() < base + size) {
        return true;
    }
    return false;
}

auto VmaSet::find_spot(usize size, usize align, VirtAddr low_limit, VirtAddr high_limit) const
    -> ustl::Result<VirtAddr, Status> {
    if (!size) {
        return ustl::err(Status::InvArgs);
    }
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }

    // Walk the candidate gaps in address order and take the first that
    // holds the request, which keeps the lowest-address behaviour the
    // previous implementation had.
    //
    // The walk is pruned rather than exhaustive: a subtree whose largest
    // internal gap cannot hold the request is skipped whole, and only the
    // seam between it and what came before is examined. `prev_end` carries
    // the end of the last area seen across the whole traversal, so the gaps
    // before the first area and after the last one fall out of the same
    // bookkeeping instead of needing separate cases.
    struct GapSeeker final {
        /// Candidate placement at the head of the current gap, checked
        /// against the alignment, the window, and the gap itself.
        auto fits(VirtAddr gap_start, VirtAddr gap_end) -> bool {
            auto const start = ustl::mm::align_up(std::max(gap_start, m_floor), m_align);
            if (start < gap_start) {
                return false; // alignment overflowed
            }
            if (start > m_high_limit || m_high_limit - start < m_requested_size) {
                return false;
            }
            if (gap_end < start || gap_end - start < m_requested_size) {
                return false;
            }
            m_found = start;
            return true;
        }

        auto on_subtree(VmAreaOrMapping::GapAugment::Aggregate const &aggregate)
            -> Tree::VisitStep {
            if (aggregate.m_empty) {
                return Tree::VisitStep::Skip;
            }
            if (aggregate.m_max_gap >= m_requested_size) {
                // Something inside might fit; the descent also covers the
                // seam leading into it.
                return Tree::VisitStep::Descend;
            }
            // Nothing inside is large enough, so only the seam before this
            // subtree can still work.
            if (fits(m_prev_end, aggregate.m_min_start)) {
                return Tree::VisitStep::Stop;
            }
            if (aggregate.m_max_end > m_prev_end) {
                m_prev_end = aggregate.m_max_end;
            }
            return Tree::VisitStep::Skip;
        }

        auto on_value(VmAreaOrMapping *area) -> bool {
            if (fits(m_prev_end, area->base())) {
                return false;
            }
            if (area->end() > m_prev_end) {
                m_prev_end = area->end();
            }
            return true;
        }

        usize m_requested_size;
        usize m_align;
        VirtAddr m_floor;
        VirtAddr m_high_limit;
        VirtAddr m_found{};
        /// End of the last area seen so far, carried across subtree
        /// boundaries so the gaps before the first area and after the last
        /// one fall out of the same bookkeeping.
        VirtAddr m_prev_end;
    };

    VirtAddr const floor = ustl::mm::align_up(low_limit, PAGE_SIZE);
    auto seeker = GapSeeker{
        .m_requested_size = ustl::mm::align_up(size, align),
        .m_align = align,
        .m_floor = floor,
        .m_high_limit = high_limit,
        .m_prev_end = floor,
    };

    if (!m_tree.visit_ordered(seeker)) {
        // Only a found spot stops the seeker mid-walk.
        return ustl::ok(seeker.m_found);
    }
    // Whatever is left between the last area and the top of the window.
    if (seeker.fits(seeker.m_prev_end, high_limit)) {
        return ustl::ok(seeker.m_found);
    }
    return ustl::err(Status::NotFound);
}

VmAreaOrMapping::Builder::Builder(ustl::Arc<VmAspace> *aspace, VmaFlags vmaf, usize size)
    : m_size(size), m_vmaf(vmaf), m_aspace(aspace) {}

auto VmAreaOrMapping::Builder::build(ustl::Arc<VmAreaOrMapping> *out) -> Status {
    if (!out) {
        return Status::InvArgs;
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
