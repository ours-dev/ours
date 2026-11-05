#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>

#include <algorithm>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
namespace {
    constexpr auto valid_map_options(MapOptions const options) -> bool {
        return !(options & ~MapOptions::ValidMask) &&
               (options & MapOptions::ModeMask) != MapOptions::ModeMask;
    }
} // namespace

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
INIT_CODE
static auto init_vma_cache() -> void {
    if (Status::Ok != VmaCache::init_from_option()) {
        panic("Failed to create the object cache for VmArea");
    }
    log::trace("VmaCache has been created");
}
GKTL_INIT_HOOK(VmaCacheInit, init_vma_cache, gktl::InitLevel::Heap);

VmArea::VmArea(VirtAddr base, usize size, VmaFlags vmaf, Self *parent, VmAspace *aspace,
               char const *name)
    : Base(gktl::magic<Self>(), base, size, vmaf, parent, aspace, name)
    , m_subvmas() {}

auto VmArea::create_common(VirtAddr base, usize size, VmaFlags vmaf, VmArea *parent,
                           VmAspace *aspace, char const *name, ustl::Arc<Self> *out) -> Status {
    DEBUG_ASSERT(aspace, "Given a invalid aspace");
    DEBUG_ASSERT(out, "`out` can not be null");

    auto self = VmaCache::get(ALLOC_KERNEL, base, size, vmaf, parent, aspace, name);
    if (!self) {
        return Status::OutOfMem;
    }
    if (parent) {
        parent->get_lock().assert_held();
        if (auto const status = parent->link_child_locked(*self); Status::Ok != status) {
            VmaCache::put(self);
            return status;
        }
    }
    self->mark_active();

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

auto VmArea::create_subvma(VirtAddr where, usize requested_size, VmaFlags vmaf, char const *name,
                           ustl::Arc<VmArea> *out) -> Status {
    m_canary.verify<Self>();

    // The new area's allowance, which may narrow this one's but never widen
    // it. This used to be checked as validate_vmaflags(m_vmaf) -- m_vmaf
    // against m_vmaf, true for every input -- and |vmaf| was then dropped on
    // the floor and the child built with m_vmaf as well. Every area in the
    // kernel address space therefore inherited the root VMA's
    // ROOT_VMAF = PermMask, and the nine per-segment allowances init_vmm.cpp
    // passes here (k:code exec, k:rodata read-only, and so on) never existed
    // at run time. Page tables were unaffected -- reserve() sends its own
    // MmuFlags down -- but validate_mmuflags() nodded at everything, so the
    // check pass in VmArea::protect() could never refuse a request.
    if (!validate_vmaflags(vmaf)) {
        return Status::InvArgs;
    }

    // From finding the spot to linking the new area into this one's set, all
    // under the address space lock; see resolve_range_locked().
    Guard guard{get_lock()};
    auto const result = resolve_range_locked(where, requested_size);
    if (result.is_err()) {
        return result.unwrap_err();
    }

    auto const [base, size] = *result;

    // No VMO offset here: a sub-area is not backed by an object at all, and
    // Builder ignores the field unless a VMO was set. It used to be handed the
    // *VMA* offset, which is the mix-up the absolute-address convention exists
    // to stop -- harmless here only by accident.
    ustl::Arc<VmAreaOrMapping> aom;
    auto const status = Builder(&m_aspace, vmaf, size)
                            .set_parent(this)
                            .set_base(base)
                            .set_name(name)
                            .build(&aom);

    if (Status::Ok != status) {
        return status;
    }
    *out = ustl::downcast<VmArea>(ustl::move(aom));

    return Status::Ok;
}

auto VmArea::create_mapping(VirtAddr where, usize requested_size, usize vmo_ofs, MmuFlags mmuf,
                            VmaFlags ceiling, ustl::Arc<VmObject> vmo, char const *name,
                            MapOptions option, ustl::Arc<VmMapping> *out) -> Status {
    m_canary.verify<Self>();

    if (!vmo || !ustl::is_aligned(vmo_ofs, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    // What this area permits, asked of the protection the mapping is being
    // created with. Nothing asked it before: the preamble's check was handed
    // m_vmaf and so compared this area against itself, and |mmuf| went
    // straight into the page tables unexamined. reserve() has always run this
    // check; the two ways of creating a mapping now agree.
    if (!validate_mmuflags(mmuf)) {
        return Status::InvArgs;
    }

    // The mapping's own ceiling: what the caller asks, cut down to what this
    // area allows. The protection it starts with has to fit under it, or the
    // mapping would begin above the line protect() is held to.
    auto const allowed = ceiling & permissions();
    if ((permissions_of(mmuf) & ~allowed) != VmaFlags::None) {
        return Status::InvArgs;
    }

    // Where in a block the mapping starts, for an object that takes blocks:
    // see resolve_range_locked().
    ustl::Option<usize> block_phase;
    if (!!(vmo->flags() & VmoFlags::LargePages)) {
        block_phase = ustl::some(static_cast<usize>(vmo_ofs % VmCowPages::BLOCK_SIZE));
    }

    // From finding the spot to linking the mapping into this area's set and
    // its VMO's list, all under the address space lock; see
    // resolve_range_locked(). The VMO's own lock is taken inside it, by
    // VmMapping::activate(), in the one order the two are ever taken.
    Guard guard{get_lock()};
    auto const result = resolve_range_locked(where, requested_size, block_phase);
    if (result.is_err()) {
        return result.unwrap_err();
    }

    auto const [base, size] = *result;
    auto const vmo_size = vmo->size();
    if (size > vmo_size || vmo_ofs > vmo_size - size) {
        return Status::OutOfRange;
    }

    // A mapping's VmaFlags are the ceiling a later protect() is checked
    // against: this area's own flags, with the permission bits narrowed to
    // |allowed|. |mmuf| is only where the mapping starts out, which is why the
    // two are separate arguments and only one of them is an allowance. It used
    // to be m_vmaf whole, so every mapping could be protect()ed up to whatever
    // its area allowed -- a read-only BootFS mapping to writable (interface
    // design 2.3).
    // Nor does a mapping take the area's CanMapSpecific: it is a rule about
    // what goes into an area, and nothing goes into a mapping.
    auto vmaf = (m_vmaf & ~(VmaFlags::PermMask | VmaFlags::CanMapSpecific)) | allowed;
    if (!!(option & MapOptions::Sealed)) {
        vmaf |= VmaFlags::Sealed;
    }
    ustl::Arc<VmAreaOrMapping> out_aom;
    auto const status = Builder(&m_aspace, vmaf, size)
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
                            .build(&out_aom);

    if (Status::Ok != status) {
        return status;
    }
    *out = ustl::downcast<VmMapping>(ustl::move(out_aom));
    return Status::Ok;
}

auto VmArea::link_child_locked(VmAreaOrMapping &child) -> Status {
    DEBUG_ASSERT(child.m_parent == this);
    if (!is_active()) {
        return Status::BadState;
    }
    return m_subvmas.insert(child);
}

auto VmArea::unlink_child_locked(VmAreaOrMapping &child) -> void {
    DEBUG_ASSERT(child.m_parent == this);
    child.m_parent = nullptr;
    m_subvmas.erase(child);
}

/// Picks where a new child goes. Called with the address space lock held, and
/// the caller keeps holding it until the child is linked into m_subvmas.
///
/// The spot found here is only free while nobody else can insert: this reads
/// m_subvmas and the caller's activate() writes it. Both ran with no lock at
/// all, so two threads creating in one area could pick the same spot, and
/// their inserts wrote the set concurrently. The address space lock now
/// covers the whole span, from this search to that insert.
auto VmArea::resolve_range_locked(VirtAddr where, usize size, ustl::Option<usize> const block_phase)
    -> ktl::Result<ustl::Pair<VirtAddr, usize>> {
    if (!is_active()) {
        // Operate an illegal VMA.
        return ktl::err(Status::BadState);
    }

    // Interpret special values before alignment can turn them into an address.
    if (where != VirtAddr::Anywhere && where >= VIRT_ADDR_RESERVED_BASE) {
        return ktl::err(Status::InvArgs);
    }

    // Round the size on its own first: the two branches below disagree about
    // where the range starts but not about how long it is, and the search
    // needs the rounded length before there is an address to round with it.
    auto const size_aligned = ustl::round_up(size, PAGE_SIZE);
    if (!size_aligned) {
        return ktl::err(Status::InvArgs);
    }

    if (where == VirtAddr::Anywhere) {
        // Anywhere is a reserved address value; zero is a concrete address.
        // A mapping of an object that takes blocks goes |block_phase| past a
        // block boundary, in a spot that much longer: its addresses then agree
        // with its object's offsets modulo a block. Only addresses are spent
        // on it, never memory. With no such spot it is placed as any other,
        // and is mapped with small entries only.
        constexpr usize BLOCK = VmCowPages::BLOCK_SIZE;
        if (block_phase && size_aligned >= BLOCK && size_aligned <= ~usize{0} - *block_phase) {
            auto const padded = size_aligned + *block_phase;
            if (auto const spot = alloc_spot(padded, BLOCK, m_base, m_base + m_size - 1)) {
                return ktl::ok(ustl::make_pair(*spot + *block_phase, size_aligned));
            }
        }

        auto const spot = alloc_spot(size_aligned, PAGE_SIZE, m_base, m_base + m_size - 1);
        if (!spot) {
            return ktl::err(spot.unwrap_err());
        }
        return ktl::ok(ustl::make_pair(*spot, size_aligned));
    }

    auto const [base_aligned, aligned] = resolve_page_range(where, size);
    if (!aligned) {
        return ktl::err(Status::InvArgs);
    }

    // A place the caller chose, which only an area made to take one accepts
    // (VmaFlags::CanMapSpecific). Checked after the alignment and before the
    // range, which settles the error a request breaking several rules gets.
    if (!(m_vmaf & VmaFlags::CanMapSpecific)) {
        return ktl::err(Status::AccessDenied);
    }

    if (!check_range(base_aligned, aligned)) {
        return ktl::err(Status::InvArgs);
    }

    // Occupied means refused, full stop. The flag pair this replaces had two
    // spellings of "here": FixedNoReplace returned AlreadyExists, while plain
    // Fixed tripped a DEBUG_ASSERT(false) and then, in a build with assertions
    // compiled out, *fell through* and created a second area overlapping the
    // first -- VmaSet::insert() only notices a collision when the bases match
    // exactly, so a partial overlap went in silently. Refusing is the only
    // answer the VMM can currently back up. A future replacement operation
    // needs an explicit request of its own.
    if (m_subvmas.has_range(base_aligned, aligned)) {
        return ktl::err(Status::AlreadyExists);
    }

    return ktl::ok(ustl::make_pair(base_aligned, aligned));
}

auto VmArea::map_phys(PhysAddr phys_base, VirtAddr where, usize size, MmuFlags mmuf,
                      MapOptions options, char const *name) -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify<Self>();

    if (!valid_map_options(options)) {
        return ustl::err(Status::InvArgs);
    }

    DEBUG_ASSERT(ustl::is_aligned(phys_base, PAGE_SIZE));
    size = ustl::round_up(size, PAGE_SIZE);
    if (!size) {
        return ustl::err(Status::InvArgs);
    }

    ustl::Arc<VmObjectPhysical> vmo;
    auto status = VmObjectPhysical::create(phys_base, size, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    return map_vmo(where, size, 0, mmuf, VmaFlags::PermMask, ustl::move(vmo), options, name);
}

auto VmArea::map_anon(VirtAddr where, usize size, MmuFlags mmuf, MapOptions options,
                      char const *name) -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify<Self>();

    if (!valid_map_options(options)) {
        return ustl::err(Status::InvArgs);
    }

    size = ustl::round_up(size, PAGE_SIZE);
    if (!size) {
        return ustl::err(Status::InvArgs);
    }

    ustl::Arc<VmObjectPaged> vmo;
    auto status = VmObjectPaged::create(ALLOC_KERNEL, size, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    return map_vmo(where, size, 0, mmuf, VmaFlags::PermMask, ustl::move(vmo), options, name);
}

auto VmArea::map_vmo(VirtAddr where, usize size, u64 vmo_ofs, MmuFlags mmuf, VmaFlags ceiling,
                     ustl::Arc<VmObject> vmo, MapOptions option, char const *name)
    -> ktl::Result<ustl::Arc<VmMapping>> {
    m_canary.verify<Self>();

    if (!valid_map_options(option)) {
        return ustl::err(Status::InvArgs);
    }

    auto const mode = option & MapOptions::ModeMask;
    ustl::Arc<VmMapping> mapping;
    auto status = create_mapping(where, size, vmo_ofs, mmuf, ceiling, ustl::move(vmo), name,
                                 option & MapOptions::Sealed, &mapping);
    if (Status::Ok != status) {
        return ustl::err(status);
    }

    if (mode != MapOptions::Lazy) {
        status = mapping->map(mapping->base(), mapping->size(), mode);

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
            // Under the address space lock, as every other destroy() runs:
            // create_mapping() let go of it on return, and map() takes and
            // drops it on its own.
            auto const teardown = [&] {
                Guard guard{get_lock()};
                mapping->get_lock().assert_held();
                return mapping->destroy();
            }();
            if (Status::Ok != teardown) {
                log::error("Failed to tear down the mapping at {} after a failed map: {}",
                           mapping->base(), to_string(teardown));
            }
            return ustl::err(status);
        }
    }

    // No out-parameter to fill: the mapping carries its own address, and
    // mapping->base() is the one answer rather than a copy of it that a caller
    // could go on using after the mapping is gone.
    return ustl::ok(ustl::move(mapping));
}

auto VmArea::unmap(VirtAddr base, usize size, UnmapScope const scope) -> Status {
    m_canary.verify<Self>();

    // A length that rounds up to nothing -- 0, or one so close to 2^64 that
    // rounding wraps -- is refused before the lock: the answer depends on
    // nothing the lock protects.
    auto [aligned_base, aligned_size] = resolve_page_range(base, size);
    if (!aligned_size) {
        return Status::InvArgs;
    }

    Guard guard{get_lock()};

    // Asked under the lock, because destroy() clears Active under it. Asked
    // before taking it, the answer could be stale by the time the walk below
    // ran: a program can now unmap a sub-area through its parent while
    // another thread unmaps inside it through the sub-area's own handle, and
    // the second call would find the sub-area emptied and answer Ok where it
    // should answer BadState.
    if (!is_active()) {
        return Status::BadState;
    }

    // The offset form needed a `offset > m_size` guard ahead of this, purely to
    // keep `m_base + offset` from wrapping below its own base. check_range() is
    // wrap-safe on its own -- it subtracts rather than adds -- so an absolute
    // address needs no such preamble.
    if (!check_range(aligned_base, aligned_size)) {
        return Status::InvArgs;
    }

    // A sealed mapping anywhere in the range -- in this area or in a sub-area
    // below it, partly or wholly covered -- refuses the whole operation,
    // before anything is torn down: the same asked-first rule protect() keeps,
    // and for the same reason, that half an unmap cannot be undone.
    if (has_sealed_mapping_locked(aligned_base, aligned_size)) {
        return Status::AccessDenied;
    }
    return unmap_locked(aligned_base, aligned_size, scope);
}

auto VmArea::destroy_child(VmAreaOrMapping &child) -> Status {
    m_canary.verify<Self>();

    Guard guard{get_lock()};

    // Still linked here: in this area's set, at its base. Asked under the
    // lock every destroy() runs under, so the answer holds until the destroy
    // below; and asked of the set itself, not of the child's Active bit and
    // parent, which say the same only while every insert succeeds. An object
    // whose insert failed has both and is in no set
    // (docs/repairs/261002/mm.md, item 2), and destroy() would then erase
    // whatever the set holds at its base. The object is the caller's to keep
    // alive, which is what makes the question safe to ask: a held reference
    // keeps its memory from going to a new object, which a range, or a bare
    // address, would not tell apart from it.
    auto const found = m_subvmas.lower_bound(child.base());
    if (found.is_none() || *found.value() != &child) {
        return Status::BadState;
    }
    DEBUG_ASSERT(child.m_parent == this && child.is_active(), "a linked child is not ours");

    // The same lock as ours; see VmAreaOrMapping::get_lock().
    child.get_lock().assert_held();
    return child.destroy();
}

/// Whether any mapping under this area that overlaps [base, base + size) is
/// sealed. The overlap is what counts, not coverage: trimming a sealed
/// mapping changes it as surely as removing it. A walk over the mappings the
/// range touches, one enumerator however deep the areas nest, as protect()'s.
auto VmArea::has_sealed_mapping_locked(VirtAddr const base, usize const size) -> bool {
    MappingEnumerator enumerator{*this, base, base + size - 1};
    while (auto *const mapping = enumerator.next()) {
        if (mapping->is_sealed()) {
            return true;
        }
    }
    return false;
}

/// Tears down everything in [base, base + size), refusing a range that only
/// partly covers a sub-area.
///
/// The refusal is what lets this stop recursing, and the two are not separable.
/// This used to descend into a partly-covered sub-area to trim the mappings
/// inside it, one stack frame per level of nesting, with nothing bounding that
/// nesting. Once a sub-area can only be wholly covered, there is nothing left
/// to descend for: a whole sub-area is destroyed outright, and destroy() is
/// already a cursor walk rather than a recursion.
///
/// The narrowing is defensible on its own terms, which is the reason to take it
/// rather than to build a resumable walk. A sub-area is a *reservation* of
/// address space, not a container of pages; punching a hole through one from
/// outside has no obvious answer -- does the reservation shrink, split, or stay
/// with a gap? This refuses to guess: a range that partly spans a sub-area is
/// rejected, and a caller who means to dissolve one names it and unmaps inside
/// it instead.
///
/// |scope| is the opt-out: whether destroying a whole sub-area is
/// allowed at all, so that vma_unmap can forbid what the kernel's own callers
/// are allowed. A handle without OpChildren may unmap mappings beside a
/// sub-area but not the sub-area, and is refused with InvArgs rather than
/// having the sub-area skipped: a skip would leave a range the caller asked to
/// be empty with something in it.
auto VmArea::unmap_locked(VirtAddr base, usize size, UnmapScope const scope) -> Status {
    DEBUG_ASSERT(size, "an empty range has nothing to unmap");

    // Inclusive, because `base + size` is 2^64 when the range is the whole
    // kernel root VMA and an exclusive end would wrap to 0. See
    // VmAreaOrMapping::last().
    VirtAddr const last = base + size - 1;

    // Ask before doing, for the reason protect() does: once the walk below has
    // destroyed its first child there is no putting it back, and a range that
    // clips a sub-area has to be refused as a whole rather than half applied.
    // Read-only, so a single enumerator carries it.
    {
        VmaSet::Enumerator enumerator{m_subvmas, base, last};
        while (auto *const child = enumerator.next()) {
            if (child->is_mapping()) {
                // A mapping is a run of pages and may be trimmed at either end.
                continue;
            }
            if (child->base() < base || child->last() > last || scope == UnmapScope::MappingsOnly) {
                return Status::InvArgs;
            }
        }
    }

    // One iterator carries the whole walk, and only the steps that actually
    // erase pay to re-establish it.
    //
    // Erasing is what makes this different from the read-only walks. A fully
    // covered child is destroyed here, and VmAreaOrMapping::destroy() erases it
    // from this very set; a B-tree erase can rebalance, which unseats iterators
    // anywhere in the tree, not merely the one that named what was erased. So
    // the tree has to hand back the position after the hole, which is what
    // VmaSet::erase(IterMut) is for.
    //
    // Trimming a partly covered mapping erases nothing, so that step is a plain
    // ++ at O(1) amortised. That is the gain over re-seeking per child: an
    // unmap that spans mostly-trimmed mappings costs one seek, not one per
    // child.
    auto iter = m_subvmas.seek(base);
    while (iter != m_subvmas.end()) {
        auto *const child = *iter;
        if (child->base() > last) {
            // Ordered by base, so nothing further can overlap.
            break;
        }

        if (base <= child->base() && child->last() <= last) {
            // Wholly covered, so it has no role left to play: tear it down for
            // good instead of leaving an object whose every page is unmapped.
            // Areas and mappings alike -- destroy() is virtual, and VmArea's
            // override walks its own subtree down and back up through m_parent.
            //
            // Out of the set first, because that is the only way to learn where
            // the walk resumes; destroy() cannot report it. The reference is
            // ours for the duration because erasing drops the set's, and the
            // set's may well have been the last one. destroy() erases again on
            // its way out, but VmaSet::erase() only releases when the tree
            // actually held the key, so nothing is released twice.
            child->retain();
            iter = m_subvmas.erase(iter);
            // The same lock as ours; see VmAreaOrMapping::get_lock().
            child->get_lock().assert_held();
            auto const status = child->destroy();
            child->release();
            if (Status::Ok != status) {
                return status;
            }
            continue;
        }

        // Only a mapping can still be here: the pass above refused every range
        // that leaves a sub-area partly covered.
        DEBUG_ASSERT(child->is_mapping(), "a partly covered sub-area survived the check pass");
        auto *const mapping = static_cast<VmMapping *>(child);
        VirtAddr const overlap_base = std::max(mapping->base(), base);
        VirtAddr const overlap_last = std::min(mapping->last(), last);

        // |overlap_base| straight through: VmMapping::unmap_locked() takes an
        // absolute address, the same as its protect_locked() sibling and the
        // same as what VmMapping::unmap() computes before calling it.
        mapping->get_lock().assert_held();
        auto const status = mapping->unmap_locked(overlap_base, overlap_last - overlap_base + 1,
                                                  UnmapOptions::None);
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
    Guard guard{get_lock()};

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

    // Ask before doing. A protect that fails halfway would leave the first
    // children rewritten and the rest untouched, with nothing in the return
    // value saying where the boundary fell -- and no way for the caller to put
    // it back, since the previous flags are gone. Every child that could
    // refuse is asked first, and only then is anything written. This is the
    // one place in the area interface where a partial result is unrecoverable,
    // which is why unmap() does not need the same treatment: a range that is
    // half unmapped can simply be unmapped again.
    if (has_sealed_mapping_locked(aligned_base, aligned_size)) {
        return Status::AccessDenied;
    }
    if (!can_protect_locked(aligned_base, aligned_size, mmuf)) {
        return Status::InvArgs;
    }

    return protect_locked(aligned_base, aligned_size, mmuf);
}

VmArea::MappingEnumerator::MappingEnumerator(VmArea &root, VirtAddr first, VirtAddr last)
    : m_root(&root)
    , m_area(&root)
    , m_iter(root.m_subvmas.seek(first))
    , m_first(first)
    , m_last(last) {}

auto VmArea::MappingEnumerator::next() -> VmMapping * {
    while (true) {
        if (m_iter == m_area->m_subvmas.end() || (*m_iter)->base() > m_last) {
            // This level is spent -- either out of children, or into ones that
            // start past the window, which the base ordering says nothing after
            // can undo.
            if (m_area == m_root) {
                return nullptr;
            }

            auto *const finished = m_area;
            m_area = finished->m_parent;

            // Re-found, not remembered. Remembering a position per level is
            // precisely the stack this class exists not to have, and the tree
            // is keyed by base, so seek() lands exactly on the child we came
            // out of: lower_bound() matches its key, and the step-back cannot
            // fire because the sibling before it ends below that base.
            m_iter = m_area->m_subvmas.seek(finished->base());
            DEBUG_ASSERT(m_iter != m_area->m_subvmas.end() && *m_iter == finished,
                         "the area the walk descended into is no longer in its parent");
            ++m_iter;
            continue;
        }

        auto *const child = *m_iter;
        if (child->is_mapping()) {
            // Past it before handing it over, so a caller may drop its own
            // reference to what it receives -- the same contract
            // VmaSet::Enumerator offers.
            ++m_iter;
            return static_cast<VmMapping *>(child);
        }

        // Descend, leaving |m_iter| where it is: the climb above steps past
        // this child by re-finding it, so advancing here would skip a sibling.
        m_area = static_cast<VmArea *>(child);

        // Seeking from |m_first| rather than from the sub-area's own base is
        // right in both directions: a window opening inside the sub-area picks
        // up the straddler, and one opening below it lands on the first child,
        // because every child's base is then above |m_first| and seek()'s
        // step-back cannot fire at begin().
        m_iter = m_area->m_subvmas.seek(m_first);
    }
}

VmAreaOrMapping::NodeEnumerator::NodeEnumerator(VmArea &root, usize const first_depth)
    : m_root(&root), m_area(&root), m_iter(root.m_subvmas.begin()), m_depth(first_depth) {}

auto VmAreaOrMapping::NodeEnumerator::next() -> Node {
    while (true) {
        if (m_iter == m_area->m_subvmas.end()) {
            if (m_area == m_root) {
                return Node{nullptr, 0};
            }
            // Climb, and re-find the area just finished by its base, as
            // MappingEnumerator does, to step past it.
            auto *const finished = m_area;
            m_area = finished->m_parent;
            --m_depth;
            m_iter = m_area->m_subvmas.seek(finished->base());
            DEBUG_ASSERT(m_iter != m_area->m_subvmas.end() && *m_iter == finished,
                         "the area the walk descended into is no longer in its parent");
            ++m_iter;
            continue;
        }

        auto *const child = *m_iter;
        auto const depth = m_depth;
        if (child->is_mapping()) {
            ++m_iter;
            return Node{child, depth};
        }

        // An area is handed out now and descended into on the next call;
        // |m_iter| in the parent is left behind, since the climb re-finds the
        // area instead of remembering where it was.
        m_area = static_cast<VmArea *>(child);
        m_iter = m_area->m_subvmas.begin();
        ++m_depth;
        return Node{child, depth};
    }
}

/// Not const, though it changes nothing: MappingEnumerator only walks a mutable
/// tree. Giving it a const form is worth doing once a second read-only range
/// walk needs one.
auto VmArea::can_protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> bool {
    DEBUG_ASSERT(size, "an empty range has nothing to protect");

    // Only mappings can refuse: a sub-area holds no page tables, so the old
    // recursion descended into one purely to reach the leaves under it. The
    // enumerator does that descent, so what is left is one flat pass.
    MappingEnumerator enumerator{*this, base, base + size - 1};
    while (auto *const mapping = enumerator.next()) {
        if (!mapping->validate_mmuflags(mmuf)) {
            return false;
        }
    }

    return true;
}

auto VmArea::protect_locked(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    DEBUG_ASSERT(size, "an empty range has nothing to protect");
    VirtAddr const last = base + size - 1;

    // A single enumerator carries the whole walk, however deeply the areas
    // nest: protect never destroys a child, so nothing is erased from under the
    // iterator, and the descent needs no stack because m_parent is the way
    // back. See VmArea::MappingEnumerator.
    MappingEnumerator enumerator{*this, base, last};

    while (auto *const mapping = enumerator.next()) {
        VirtAddr const overlap_base = std::max(mapping->base(), base);
        VirtAddr const overlap_last = std::min(mapping->last(), last);

        mapping->get_lock().assert_held();
        auto const status =
            mapping->protect_locked(overlap_base, overlap_last - overlap_base + 1, mmuf);
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
    auto status = VmObjectPaged::create(ALLOC_KERNEL, 0, VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    // Created under the address space lock, as create_mapping() does, and
    // released before protect() below takes it again for itself. A
    // reservation is a mapping at a place chosen for it, so this area must
    // take one: it meets the same CanMapSpecific check as any other mapping
    // placed at a fixed address.
    ustl::Arc<VmMapping> mapping;
    {
        Guard guard{get_lock()};
        if (!(m_vmaf & VmaFlags::CanMapSpecific)) {
            return Status::AccessDenied;
        }
        if (m_subvmas.has_range(aligned_base, size)) {
            return Status::AlreadyExists;
        }
        status = VmMapping::create(this, aligned_base, size, m_vmaf & ~VmaFlags::CanMapSpecific,
                                   ustl::move(vmo), 0, mmuf, name, &mapping);
    }
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

/// Tears down this area and everything under it, deepest first.
///
/// A cursor walk rather than a recursion, and unlike find_mapping_locked()'s
/// this one really does have branching to remember -- an area has many
/// children and each may have its own. What stands in for the stack is
/// |m_parent|: every node knows the way back, so the walk descends into the
/// first surviving child and, when a node has none left, climbs one link and
/// looks again. No frame, no array, and no depth for a caller to overrun.
auto VmArea::destroy() -> Status {
    m_canary.verify<Self>();

    auto first_error = Status::Ok;
    ustl::Arc<Self> curr(this);
    while (curr) {
        while (!curr->m_subvmas.empty()) {
            ustl::Arc<Base> aom(*curr->m_subvmas.begin());
            if (!aom->is_mapping()) {
                // An area of its own, so go down instead of destroying it here:
                // Base::destroy() would unlink it while its own children were
                // still pointing at it.
                curr = ustl::adopt<Self, ustl::sync::ThreadSafePolicy>(aom.as_ptr_mut());
                continue;
            }

            // Carried on past a failure, and the first one reported. It used to
            // return here, which left the rest of the tree mapped and alive --
            // under an address space that went on to consider itself torn
            // down. Carrying on always makes progress: VmMapping::destroy()
            // takes the mapping out of this set whatever its unmap did, and
            // the one answer that leaves it linked -- BadState, already
            // destroyed -- cannot come from a mapping still in a set.
            aom->get_lock().assert_held();
            if (auto const status = aom->destroy(); Status::Ok != status) {
                DEBUG_ASSERT(status != Status::BadState, "a destroyed mapping was still linked");
                if (Status::Ok == first_error) {
                    first_error = status;
                }
            }
        }

        // Read the way back before unlinking, because Base::destroy() clears
        // m_parent -- and read it off |curr|, not off |this|. It used to be
        // spelt `m_parent`, which is this area's parent no matter how far the
        // cursor had descended: on the kernel root VMA that is null, so the
        // walk destroyed the first leaf area it reached, climbed to nowhere,
        // and returned Ok with the rest of the tree still alive. The two
        // spellings compile the same and differ only when the walk is more than
        // one level deep, which is why the shape of the loop hid it.
        auto *const parent = curr->m_parent;
        curr->get_lock().assert_held();
        // Unlinking cannot fail: VmAreaOrMapping::destroy() answers only Ok.
        auto _ = curr->Base::destroy();
        if (curr.as_ptr() == this) {
            break;
        }

        // Traverse up.
        curr = parent ? ustl::adopt<Self, ustl::sync::ThreadSafePolicy>(parent) : ustl::Arc<Self>();
    }

    return first_error;
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

    // A cursor, not a call. This used to recurse into the sub-area it found,
    // one frame per level of nesting, and nothing bounds that nesting:
    // create_subvma() will build a sub-area inside a sub-area as deep as it is
    // asked to. The kernel aspace happens to be two levels deep, but a user
    // aspace's shape is the user's to choose, and this is the lookup on the
    // page-fault path -- the frames would land on whichever stack took the
    // fault, at a depth chosen by the faulting program.
    //
    // Nothing is lost by flattening it: the descent only ever follows one
    // child, because the children are disjoint and an address is a one-byte
    // window, so at most one of them can answer. There is no branching to
    // remember and therefore no stack to keep.
    auto *area = this;
    while (true) {
        // First and last are the same address; see the window's shape in
        // VmAreaOrMapping::last().
        VmaSet::Enumerator enumerator{area->m_subvmas, va, va};
        auto *const child = enumerator.next();
        if (!child) {
            return nullptr;
        }

        if (child->is_mapping()) {
            return static_cast<VmMapping *>(child);
        }
        area = static_cast<VmArea *>(child);
    }
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
