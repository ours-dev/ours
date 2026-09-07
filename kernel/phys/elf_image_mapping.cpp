/// ElfImage::fix_mapping() only.
///
/// Split out of elf-image.cpp because it is the one ElfImage method that
/// touches Aspace -- calling aspace.map() needs Aspace::map()'s
/// implementation (aspace.cpp) linked in, which in turn needs BootMem
/// (global_bootmem()).  physload has both; boot-shim, which links
/// elf-image.cpp on its own to load physload, has neither and never calls
/// fix_mapping (it runs identity-mapped already).  Keeping this in its own
/// translation unit means boot-shim's build never has to satisfy those
/// symbols just because they live in the same file.

#include "detail/numeric.hpp"
#include <ours/phys/aspace.hpp>
#include <ours/phys/elf_image.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>
#include <limits>

namespace ours::phys {
// Named PAGE_BYTES, not PAGE_SIZE: the latter is an object-like macro,
// so the definition would expand into itself.
namespace {
    constexpr auto PAGE_BYTES = static_cast<usize>(PAGE_SIZE);
} // namespace

/// The segment walker behind ElfImage::fix_mapping(): owns the RELRO-aware
/// run walk and its running arithmetic.  Defined at namespace scope --
/// ElfImage befriends it by name, and a friend declaration never matches an
/// anonymous-namespace entity -- but only in this TU: boot-shim links
/// elf-image.cpp without this file, so nothing else can ever name the class.
class SegmentMapper final {
  public:
    SegmentMapper(Aspace &aspace, ElfImage::LoadPlan const &plan, VirtAddr target_base,
                  PhysAddr pa_load)
        : m_aspace(aspace), m_plan(plan), m_target_base(target_base), m_pa_load(pa_load) {}

    auto map_all() -> Status {
        // Map each ELF load interval independently.  Page-granularity
        // mappings cover a segment's leading and trailing partial pages,
        // while page-sized gaps remain unmapped.
        //
        // PT_GNU_RELRO is carved out of the segment that holds it and mapped
        // without write permission.  Doing that here rather than
        // re-protecting afterwards is what makes it reliable: an
        // already-mapped range can have been backed by a large page that the
        // RELRO interval only partly covers, and there is nothing left to
        // split it with.
        if (m_target_base % PAGE_SIZE != 0 || m_pa_load % PAGE_SIZE != 0) {
            return Status::InvArgs;
        }

        // The plan reports RELRO in the image's own addresses; every range
        // has to survive the shift to `target_base` before the segment walk
        // can use it.
        for (auto const &interval: m_plan.relro_intervals()) {
            auto const end_offset =
                detail::checked_to_usize(interval.virtual_end - m_plan.image_base());
            if (end_offset.is_none() || end_offset.value() > m_plan.image_size() ||
                m_target_base > std::numeric_limits<VirtAddr>::max() - end_offset.value()) {
                return Status::InvArgs;
            }
        }

        for (auto const &segment: m_plan.segments()) {
            auto const status = map_segment(segment);
            if (status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

  private:
    using LoadSegment = ElfImage::LoadPlan::LoadSegment;

    /// A half-open run of whole pages; empty when first == last.
    struct PageRun final {
        VirtAddr first{0};
        VirtAddr last{0};
    };

    [[nodiscard]]
    static constexpr auto map_permissions(eftl::elf::SegPerms permissions) noexcept -> MmuFlags {
        auto flags = MmuFlags::Present;
        if (static_cast<unsigned>(permissions & eftl::elf::SegPerms::Readable) != 0U) {
            flags |= MmuFlags::Readable;
        }
        if (static_cast<unsigned>(permissions & eftl::elf::SegPerms::Writable) != 0U) {
            flags |= MmuFlags::Writable;
        }
        if (static_cast<unsigned>(permissions & eftl::elf::SegPerms::Executable) != 0U) {
            flags |= MmuFlags::Executable;
        }
        return flags;
    }

    /// One RELRO range clipped to one load segment and rounded to whole
    /// pages, all in target addresses.
    ///
    /// The start rounds up.  RELRO does not have to begin its segment -- this
    /// repo's kernel keeps `.init_array` behind `.init.data` -- so the page it
    /// shares with whatever precedes it must stay writable.
    ///
    /// The end rounds up only where RELRO runs to the end of the segment's
    /// content: the rest of that page is then padding the segment owns and
    /// nothing ever writes, and rounding down instead would give up the
    /// protection entirely for a segment as small as the one holding
    /// `.dynamic`.  Anywhere else it rounds down, because live writable bytes
    /// follow it inside that page.
    ///
    /// Either rounding can leave the run empty, which simply means this image
    /// cannot be protected at page granularity there.
    [[nodiscard]]
    static auto clip_relro(VirtAddr const relro_first, VirtAddr const relro_last,
                           VirtAddr const segment_first, VirtAddr const segment_limit,
                           VirtAddr const segment_last) noexcept -> PageRun {
        auto const first = ustl::mm::align_up(std::max(relro_first, segment_first), PAGE_BYTES);
        auto last = VirtAddr{0};
        if (std::min(relro_last, segment_limit) >= segment_limit) {
            last = segment_last;
        } else {
            last = ustl::mm::align_down(std::min(relro_last, segment_limit), PAGE_BYTES);
        }
        if (first >= last) {
            return {};
        }
        return {.first = first, .last = last};
    }

    auto map_segment(LoadSegment const &segment) -> Status {
        auto const &region = segment.region;
        if (region.image_offset > m_plan.image_size() ||
            segment.memory_size > m_plan.image_size() - region.image_offset ||
            m_target_base > std::numeric_limits<VirtAddr>::max() - region.image_offset ||
            m_pa_load > std::numeric_limits<PhysAddr>::max() - region.image_offset) {
            return Status::InvArgs;
        }
        auto const native_size = detail::checked_to_usize(segment.memory_size);
        if (native_size.is_none()) {
            return Status::InvArgs;
        }
        auto const virtual_address = m_target_base + region.image_offset;
        auto const physical_address = m_pa_load + region.image_offset;
        auto const page_offset = virtual_address % PAGE_BYTES;
        if (page_offset != physical_address % PAGE_BYTES ||
            native_size.value() > std::numeric_limits<usize>::max() - page_offset) {
            return Status::InvArgs;
        }
        auto const pages = detail::page_count(native_size.value() + page_offset);
        if (pages.is_none()) {
            return Status::InvArgs;
        }

        auto const virtual_page = virtual_address - page_offset;
        auto const physical_page = physical_address - page_offset;
        // `pages`, not `pages - 1`: the carving below works with the
        // segment's exclusive end address, so that address has to be
        // representable too, not just its last page.
        if (pages.value() > (std::numeric_limits<VirtAddr>::max() - virtual_page) / PAGE_BYTES ||
            pages.value() > (std::numeric_limits<PhysAddr>::max() - physical_page) / PAGE_BYTES) {
            return Status::InvArgs;
        }
        auto const segment_end = virtual_page + (pages.value() * PAGE_BYTES);
        // Where the segment's content stops, before page rounding -- what
        // tells clip_relro() whether the tail of a page is padding or data.
        auto const segment_limit = virtual_address + native_size.value();
        auto const permissions = map_permissions(region.permissions);

        // Walk the segment in runs of pages, breaking wherever a RELRO range
        // starts or ends, and map each run in one call.  One call per run
        // rather than one per page keeps large-page mappings available
        // everywhere RELRO does not actually split the segment.
        auto run_first = virtual_page;
        while (run_first < segment_end) {
            auto run_last = segment_end;
            auto run_permissions = permissions;
            for (auto const &interval: m_plan.relro_intervals()) {
                auto const relro_first =
                    m_target_base + (interval.virtual_start - m_plan.image_base());
                auto const relro_last =
                    m_target_base + (interval.virtual_end - m_plan.image_base());
                auto const run =
                    clip_relro(relro_first, relro_last, virtual_page, segment_limit, segment_end);
                if (run.first >= run.last) {
                    continue;
                }
                if (run.first <= run_first && run_first < run.last) {
                    run_permissions = permissions & ~MmuFlags::Writable;
                    run_last = std::min(run_last, run.last);
                } else if (run_first < run.first) {
                    run_last = std::min(run_last, run.first);
                }
            }

            auto const status = map_run(
                run_first, run_last, physical_page + (run_first - virtual_page), run_permissions);
            if (status != Status::Ok) {
                return status;
            }
            run_first = run_last;
        }
        return Status::Ok;
    }

    auto map_run(VirtAddr first, VirtAddr last, PhysAddr physical, MmuFlags permissions) -> Status {
        auto const mapped = m_aspace.map(first, (last - first) / PAGE_BYTES, physical, permissions);
        if (mapped.is_err()) {
            return Status::InvArgs;
        }
        return Status::Ok;
    }

    Aspace &m_aspace;
    ElfImage::LoadPlan const &m_plan;
    VirtAddr m_target_base;
    PhysAddr m_pa_load;
};

auto ElfImage::fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status {
    return SegmentMapper(aspace, m_load_plan, target_base, m_pa_load).map_all();
}

} // namespace ours::phys
