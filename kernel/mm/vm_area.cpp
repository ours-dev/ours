#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>

namespace ours::mm {
using VmaCache = ObjectCache::Instance<
    VmArea,
    ObjectCache::Option::AddInitHook<"VmaCache", ObjectCache::Flags::Folio, gktl::InitLevel::Heap>>;

/// AddInitHook only *records* the name, flags and level -- it cannot emit the
/// hook itself, because a hook declared inside a class template is a static
/// data member and would never be instantiated. The namespace-scope hook is
/// the caller's to write, and this one was missing: VmaCache::CACHE stayed
/// null, and the first VmArea::create_common() -- from
/// VmAspace::init_kernel_aspace(), the first thing init_vmm() does -- tripped
/// "ObjectCache for this type is not initialized".
INIT_CODE static auto init_vma_cache() -> void {
    if (Status::Ok != VmaCache::init_from_option()) {
        panic("Failed to create the object cache for VmArea");
    }
    log::trace("VmaCache has been created");
}
GKTL_INIT_HOOK(VmaCacheInit, init_vma_cache, gktl::InitLevel::Heap);

VmArea::VmArea(VirtAddr base, usize size, VmaFlags vmaf, Self *parent, VmAspace *aspace,
               char const *name)
    : Base(gktl::magic<Self>(), base, size, vmaf, parent, aspace, name)
    , m_num_mappings()
    , m_subvmas() {}

auto VmArea::create_common(VirtAddr base, usize size, VmaFlags vmaf, VmArea *parent,
                           VmAspace *aspace, char const *name, ustl::Arc<Self> *out) -> Status {
    DEBUG_ASSERT(aspace, "Given a invalid aspace");
    DEBUG_ASSERT(out, "`out` can not be null");

    auto self = VmaCache::get(GAF_KERNEL, base, size, vmaf, parent, aspace, name);
    if (!self) {
        return Status::OutOfMem;
    }
    if (!parent) {
        // A root VMA has no parent so just mark it active.
        self->mark_active();
    } else {
        self->activate();
    }

    *out = ustl::Arc<VmArea>::adopt(self);
    return Status::Ok;
}

auto VmArea::alloc_spot(usize size, usize align, VirtAddr lower_limit, VirtAddr upper_limit)
    -> ktl::Result<VirtAddr> {
    m_canary.verify<Self>();
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }
    return m_subvmas.find_spot(size, align, lower_limit, upper_limit);
}

auto VmArea::create_subvma(VirtAddr requested_base, usize requested_size, VmaFlags vmaf,
                           char const *name, VmMapOption option, ustl::Arc<VmArea> *out) -> Status {
    m_canary.verify<Self>();

    auto const result = create_subaom_preamble(requested_base, requested_size, m_vmaf, option);
    if (result.is_err()) {
        return result.unwrap_err();
    }

    auto const [base, size] = *result;

    // No VMO offset here: a sub-area is not backed by an object at all, and
    // Builder ignores the field unless a VMO was set. It used to be handed the
    // *VMA* offset, which is the mix-up the absolute-address convention exists
    // to stop -- harmless here only by accident.
    ustl::Arc<VmAreaOrMapping> aom;
    auto const status = Builder(&m_aspace, m_vmaf, size)
                            .set_parent(this)
                            .set_base(base)
                            .set_option(option)
                            .set_name(name)
                            .build(&aom);

    if (Status::Ok != status) {
        return status;
    }
    *out = ustl::downcast<VmArea>(ustl::move(aom));

    return Status::Ok;
}

auto VmArea::create_mapping(VirtAddr requested_base, usize requested_size, usize vmo_ofs,
                            MmuFlags mmuf, ustl::Arc<VmObject> vmo, char const *name,
                            VmMapOption option, ustl::Arc<VmMapping> *out) -> Status {
    m_canary.verify<Self>();

    auto const result = create_subaom_preamble(requested_base, requested_size, m_vmaf, option);
    if (result.is_err()) {
        return result.unwrap_err();
    }

    auto const [base, size] = *result;

    ustl::Arc<VmAreaOrMapping> out_aom;
    auto const status = Builder(&m_aspace, m_vmaf, size)
                            .set_parent(this)
                            .set_base(base)
                            .set_vmo(ustl::move(vmo))
                            // |vmo_ofs|, not the address: this indexes into the
                            // object. The VMA offset used to be passed here,
                            // and |vmo_ofs| was accepted and dropped -- the two
                            // were both bare u64 and nothing at the call site
                            // could tell them apart. Latent only because every
                            // caller so far asks for offset 0 of a fresh VMO.
                            .set_vmo_offset(vmo_ofs)
                            .set_name(name)
                            .set_mmuf(mmuf)
                            .set_option(option)
                            .build(&out_aom);

    if (Status::Ok != status) {
        return status;
    }
    *out = ustl::downcast<VmMapping>(ustl::move(out_aom));
    m_num_mappings += 1;

    return Status::Ok;
}

auto VmArea::create_subaom_preamble(VirtAddr base, usize size, VmaFlags vmaf, VmMapOption option)
    -> ktl::Result<ustl::Pair<VirtAddr, usize>> {
    if (!is_active()) {
        // Operate an illegal VMA.
        return ktl::err(Status::BadState);
    }

    // Check if the given MMU flags have the same or lower permissions than
    // those allowed by this VMA.
    if (!validate_vmaflags(vmaf)) {
        // Unallowed permissions.
        return ktl::err(Status::InvArgs);
    }

    auto [base_aligned, size_aligned] = resolve_page_range(base, size);
    if (!size_aligned) {
        return ktl::err(Status::InvArgs);
    }

    auto const is_fixed_noreplace = (!!(option & VmMapOption::FixedNoReplace));
    auto const is_fixed = (!!(option & VmMapOption::Fixed)) || is_fixed_noreplace;
    if (is_fixed) {
        // Only the fixed path reads |base|, so only it can be asked whether the
        // address is ours to give -- a question that could not even be spelt
        // while the parameter was an offset, since every offset looks in-range
        // until it is added to m_base. The other branch throws |base| away and
        // takes whatever alloc_spot() finds, which is the remaining ambiguity
        // in this interface: a caller with no address in mind still has to
        // invent one. See docs/repairs/260903/mm.md.
        if (!check_range(base_aligned, size_aligned)) {
            return ktl::err(Status::InvArgs);
        }

        if (m_subvmas.has_range(base_aligned, size_aligned)) {
            if (is_fixed_noreplace) {
                return ktl::err(Status::AlreadyExists);
            }

            // Overwrite the primitive regions.
            DEBUG_ASSERT(false, "Now we don't support to overwrite a group of existent pages");
        }
    } else {
        // Without a Fixed option the caller is asking the VMM to place this,
        // so |base| is only a starting point and the spot the search returns
        // replaces it.
        //
        // That assignment is what was missing: the result of alloc_spot() was
        // computed and thrown away, and the address actually used stayed at
        // the one derived from the caller's request. Callers that pass no
        // address at all arrive here naming the start of this VMA, so every
        // one of them was placed there -- and on the kernel root VMA that is
        // the physmap base, which is how the local APIC's MMIO ended up mapped
        // on top of the physmap.
        //
        // FIXME(SmallHuaZi) (base_ + size_) may causes an overflow error.
        auto const result = alloc_spot(size_aligned, PAGE_SIZE, m_base, m_base + m_size - 1);
        if (!result) {
            return ktl::err(result.unwrap_err());
        }
        base_aligned = *result;
    }

    return ktl::ok(ustl::make_pair(base_aligned, size_aligned));
}

auto VmArea::map_at(PhysAddr phys_base, VirtAddr *virt_base, usize size, MmuFlags mmuf,
                    VmMapOption options, char const *name) -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify<Self>();

    DEBUG_ASSERT(ustl::mm::is_aligned(phys_base, PAGE_SIZE));
    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return ustl::err(Status::InvArgs);
    }
    if (virt_base) {
        *virt_base = ustl::mm::align_down(*virt_base, PAGE_SIZE);
    }

    ustl::Arc<VmObjectPhysical> vmo;
    auto status = VmObjectPhysical::create(phys_base, size, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    return map_with_vmo(virt_base, size, mmuf, ustl::move(vmo), options, name);
}

auto VmArea::map(VirtAddr *base, usize size, MmuFlags mmuf, VmMapOption options, char const *name)
    -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify<Self>();

    DEBUG_ASSERT(!(options & VmMapOption::Fixed), "Now we don't support the overwritting option");
    size = ustl::mm::align_up(size, PAGE_SIZE);
    if (!size) {
        return ustl::err(Status::InvArgs);
    }
    if (base) {
        *base = ustl::mm::align_down(*base, PAGE_SIZE);
    }

    ustl::Arc<VmObjectPaged> vmo;
    auto status = VmObjectPaged::create(GAF_KERNEL, size, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    return map_with_vmo(base, size, mmuf, ustl::move(vmo), options, name);
}

auto VmArea::map_with_vmo(VirtAddr *base, usize size, MmuFlags mmuf, ustl::Arc<VmObject> vmo,
                          VmMapOption option, char const *name)
    -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify<Self>();

    // A null |base|, or one below this area, is today's spelling of "you
    // choose"; it reaches create_subaom_preamble() as this area's own start
    // and is then discarded, because none of these callers passes Fixed.
    VirtAddr const requested_base = (base && *base >= m_base) ? *base : m_base;

    ustl::Arc<VmMapping> mapping;
    auto status = create_mapping(requested_base, size, 0, mmuf, vmo, name, option, &mapping);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    // Do we immediately commit current mapping request?
    if (!!(VmMapOption::Commit & option)) {
        status = vmo->commit_range(0, size);
        if (Status::Ok == status) {
            status = mapping->map(mapping->base(), size, true, MapControl::ErrorIfExisting);
        }

        if (Status::Ok != status) {
            // The mapping is already live by this point: create_mapping() ends
            // in VmMapping::activate(), which links it into its VMO's mapping
            // list and its parent's subvma set. Simply propagating the error
            // would drop the last Arc here and run ~VmMapping on a node that is
            // still on both -- ustl's intrusive hook traps on exactly that, so
            // the failure came out as a #UD in the destructor rather than as
            // the status it started life as.
            //
            // VmMapping::destroy() is the counterpart activate() asks for in
            // its own comment: it unmaps whatever did get installed, takes the
            // mapping off the VMO list and the parent, and clears Active.
            auto const teardown = mapping->destroy();
            if (Status::Ok != teardown) {
                log::error("Failed to tear down the mapping at {} after a failed map: {}",
                           mapping->base(), to_string(teardown));
            }
            return ustl::err(status);
        }
    }

    if (base) {
        *base = mapping->base();
    }

    return ustl::ok(ustl::move(mapping));
}

auto VmArea::unmap(VirtAddr base, usize size) -> Status {
    m_canary.verify<Self>();

    if (!is_active()) {
        return Status::BadState;
    }

    // The offset form needed a `offset > m_size` guard ahead of this, purely to
    // keep `m_base + offset` from wrapping below its own base. check_range() is
    // wrap-safe on its own -- it subtracts rather than adds -- so an absolute
    // address needs no such preamble.
    auto [aligned_base, aligned_size] = resolve_page_range(base, size);
    if (!aligned_size) {
        return Status::InvArgs;
    }

    if (!check_range(aligned_base, aligned_size)) {
        return Status::InvArgs;
    }

    Guard guard{get_lock()};
    return unmap_locked(aligned_base, aligned_size);
}

auto VmArea::unmap_locked(VirtAddr base, usize size) -> Status {
    DEBUG_ASSERT(size, "an empty range has nothing to unmap");

    // Inclusive, because `base + size` is 2^64 when the range is the whole
    // kernel root VMA and an exclusive end would wrap to 0. See
    // VmAreaOrMapping::last(); Zircon's UnmapInternalLocked() computes the same
    // `end_addr_byte`.
    VirtAddr const last = base + size - 1;

    // One iterator carries the whole walk, and only the steps that actually
    // erase pay to re-establish it.
    //
    // Erasing is what makes this different from the read-only walks. A fully
    // covered mapping is destroyed here, and VmAreaOrMapping::destroy() erases
    // it from this very set; a B-tree erase can rebalance, which unseats
    // iterators anywhere in the tree, not merely the one that named what was
    // erased. So the tree has to hand back the position after the hole, which
    // is what VmaSet::erase(IterMut) is for.
    //
    // Every other step -- descending into a sub-area, trimming a mapping that
    // is only partly covered -- erases nothing, so it is a plain ++ at O(1)
    // amortised. That is the whole gain over re-seeking per child: an unmap
    // that spans mostly-trimmed children now costs one seek, not one per child.
    auto iter = m_subvmas.seek(base);
    while (iter != m_subvmas.end()) {
        auto *const child = *iter;
        if (child->base() > last) {
            // Ordered by base, so nothing further can overlap.
            break;
        }

        VirtAddr const overlap_base = std::max(child->base(), base);
        VirtAddr const overlap_last = std::min(child->last(), last);

        if (!child->is_mapping()) {
            // Unmap never destroys areas, it just descends into them.
            auto const status = static_cast<VmArea *>(child)->unmap_locked(
                overlap_base, overlap_last - overlap_base + 1);
            if (Status::Ok != status) {
                return status;
            }
            ++iter;
            continue;
        }

        auto *const mapping = static_cast<VmMapping *>(child);
        if (base <= mapping->base() && mapping->last() <= last) {
            // Fully covered, so the mapping has no role left to play: tear it
            // down for good instead of leaving an object whose every region is
            // unmapped.
            //
            // Out of the set first, because that is the only way to learn where
            // the walk resumes; destroy() cannot report it. The reference is
            // ours for the duration because erasing drops the set's, and the
            // set's may well have been the last one.
            mapping->retain();
            iter = m_subvmas.erase(iter);
            auto const status = mapping->destroy();
            mapping->release();
            if (Status::Ok != status) {
                return status;
            }
            continue;
        }

        // |overlap_base| straight through: VmMapping::unmap_locked() takes an
        // absolute address, the same as its protect_locked() sibling and the
        // same as what VmMapping::unmap() computes before calling it. This used
        // to subtract mapping->base() first and hand it an offset, which the
        // mapping then measured against its ProtectionRanges -- absolute
        // ranges -- and so trimmed a window in the bottom of the address space
        // instead of the one asked for. Nothing caught it because both readings
        // are a u64; nothing hit it because the boot never unmaps.
        auto const status = mapping->unmap_locked(overlap_base, overlap_last - overlap_base + 1,
                                                  UnmapControl::None);
        if (Status::Ok != status) {
            return status;
        }
        ++iter;
    }

    return Status::Ok;
}

/// Rewrite the protection of [base, base + size) throughout this area.
///
/// The range is the area's to describe but not to enforce: an area owns no
/// page tables of its own, only children. So this walks the children the range
/// covers and hands each the part that falls inside it, exactly as unmap()
/// does, and a nested area recurses.
auto VmArea::protect(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    m_canary.verify<Self>();

    if (!is_active()) {
        return Status::BadState;
    }

    auto [aligned_base, aligned_size] = resolve_page_range(base, size);
    if (!aligned_size) {
        return Status::InvArgs;
    }

    if (!check_range(aligned_base, aligned_size)) {
        return Status::InvArgs;
    }

    // What this area itself permits. A child may permit less, which the pass
    // below is what actually checks; this only rejects a request the area
    // could never satisfy anywhere.
    if (!validate_mmuflags(mmuf)) {
        return Status::InvArgs;
    }

    Guard guard{get_lock()};

    // Ask before doing. A protect that fails halfway would leave the first
    // children rewritten and the rest untouched, with nothing in the return
    // value saying where the boundary fell -- and no way for the caller to put
    // it back, since the previous flags are gone. Every child that could
    // refuse is asked first, and only then is anything written. This is the
    // one place in the area interface where a partial result is unrecoverable,
    // which is why unmap() does not need the same treatment: a range that is
    // half unmapped can simply be unmapped again.
    if (!can_protect_locked(aligned_base, aligned_size, mmuf)) {
        return Status::InvArgs;
    }

    return protect_locked(aligned_base, aligned_size, mmuf);
}

/// Not const, though it changes nothing: VmaSet::Enumerator only walks a
/// mutable set. Giving it a const form is worth doing once a second read-only
/// range walk needs one.
auto VmArea::can_protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> bool {
    DEBUG_ASSERT(size, "an empty range has nothing to protect");
    VirtAddr const last = base + size - 1;
    VmaSet::Enumerator enumerator{m_subvmas, base, last};

    while (auto *const child = enumerator.next()) {
        if (child->is_mapping()) {
            if (!child->validate_mmuflags(mmuf)) {
                return false;
            }
            continue;
        }

        auto const overlap_base = std::max(child->base(), base);
        auto const overlap_last = std::min(child->last(), last);
        auto *const area = static_cast<VmArea *>(child);
        if (!area->can_protect_locked(overlap_base, overlap_last - overlap_base + 1, mmuf)) {
            return false;
        }
    }

    return true;
}

auto VmArea::protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    DEBUG_ASSERT(size, "an empty range has nothing to protect");
    VirtAddr const last = base + size - 1;

    // A single enumerator carries the whole walk, unlike unmap_locked(): protect
    // never destroys a child, so nothing is erased from the set underneath it.
    VmaSet::Enumerator enumerator{m_subvmas, base, last};

    while (auto *const child = enumerator.next()) {
        VirtAddr const overlap_base = std::max(child->base(), base);
        VirtAddr const overlap_last = std::min(child->last(), last);
        auto const overlap_size = overlap_last - overlap_base + 1;

        auto status = Status::Ok;
        if (child->is_mapping()) {
            status =
                static_cast<VmMapping *>(child)->protect_locked(overlap_base, overlap_size, mmuf);
        } else {
            status = static_cast<VmArea *>(child)->protect_locked(overlap_base, overlap_size, mmuf);
        }

        if (Status::Ok != status) {
            return status;
        }
    }

    return Status::Ok;
}

auto VmArea::reserve(VirtAddr base, usize size_bytes, MmuFlags mmuf, char const *name) -> Status {
    m_canary.verify<Self>();
    auto [aligned_base, size] = resolve_page_range(base, size_bytes);
    if (!size) {
        return Status::InvArgs;
    }

    // The offset form could not ask this: `m_base + vma_off` names an address
    // inside some area, just not necessarily this one, and nothing here noticed.
    if (!check_range(aligned_base, size)) {
        return Status::InvArgs;
    }

    if (!validate_mmuflags(mmuf)) {
        return Status::InvArgs;
    }

    // TODO(SmallHuaZi) Should we provide a specific way to make these kind of zero length VMO.
    ustl::Arc<VmObjectPaged> vmo;
    auto status = VmObjectPaged::create(GAF_KERNEL, 0, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    ustl::Arc<VmMapping> mapping;
    status = VmMapping::create(this, aligned_base, size, m_vmaf, ustl::move(vmo), 0, mmuf, name,
                               &mapping);
    if (Status::Ok != status) {
        return status;
    }

    // Through the mapping, not around it. The pages are already mapped -- this
    // only has to stamp the permissions on them -- but the mapping is what owns
    // that decision: VmMapping::create() above has already recorded |mmuf| for
    // the whole range in the mapping's ProtectionRanges, and reaching past it
    // to the arch aspace would write page tables the mapping believes it
    // governs. It also picks up the one case the direct call gets wrong, where
    // a range carrying no permission at all is meant to be unmapped rather
    // than protected.
    return mapping->protect(aligned_base, size, mmuf);
}

auto VmArea::destroy() -> Status {
    m_canary.verify<Self>();

    ustl::Arc<Self> curr(this);
    while (curr) {
        while (!curr->m_subvmas.empty()) {
            ustl::Arc<Base> aom(*curr->m_subvmas.begin());
            if (!aom->is_mapping()) {
                // If not a mapping, recursive to keep destroying.
                curr = ustl::adopt<Self, ustl::sync::ThreadSafePolicy>(aom.as_ptr_mut());
                continue;
            }

            auto status = aom->destroy();
            if (Status::Ok != status) {
                return status;
            }
        }

        // Destroy current node.
        auto *const parent = m_parent;
        curr->Base::destroy();
        if (curr.as_ptr() == this) {
            break;
        }

        // Traverse up.
        curr = parent ? ustl::adopt<Self, ustl::sync::ThreadSafePolicy>(parent) : ustl::Arc<Self>();
    }

    m_num_mappings = 0;
    return Status::Ok;
}

auto VmArea::contains(VirtAddr addr) const -> bool {
    // One subtraction rather than two comparisons: unsigned wrap turns an
    // address below the base into a huge offset, which fails the same test.
    return addr - m_base < m_size;
}

/// Finds the mapping that |va| falls in, descending through sub-areas.
///
/// Children are ordered by base address, so the scan can stop at the first one
/// starting past |va| instead of walking the whole set.
auto VmArea::find_mapping_locked(VirtAddr va) -> VmMapping * {
    m_canary.verify<Self>();

    if (!contains(va)) {
        return nullptr;
    }

    // The one lookup on the page-fault path, so it is the one that most wants
    // the tree used as a tree. A single address is a one-byte window -- first
    // and last are the same address -- and the children are disjoint, so at
    // most one of them can answer.
    VmaSet::Enumerator enumerator{m_subvmas, va, va};
    auto *const child = enumerator.next();
    if (!child) {
        return nullptr;
    }

    if (child->is_mapping()) {
        return static_cast<VmMapping *>(child);
    }
    return static_cast<VmArea *>(child)->find_mapping_locked(va);
}

auto VmArea::dump_locked() const -> void {
    m_canary.verify<Self>();

    log::trace("VMA {}: [0x{:X}, 0x{:X}]", m_name, m_base, m_size);
    for (auto const *child: m_subvmas) {
        if (child->is_mapping()) {
            log::trace("  M {:<10}: [0x{:X}, 0x{:X}]", child->m_name, child->base(), child->size());
        } else {
            log::trace("  A {:<10}: [0x{:X}, 0x{:X}]", child->m_name, child->base(), child->size());
        }
    }
}

/// TODO(SmallHuaZi): should take the aspace lock and then defer, but the lock
/// is not reachable from here yet, and its current callers (init_vmm) do not
/// hold it. Left as a plain forward so the two spellings cannot drift apart.
auto VmArea::dump() const -> void {
    dump_locked();
}

} // namespace ours::mm