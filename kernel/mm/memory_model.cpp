#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memory_model.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/panic.hpp>
#include <algorithm>
namespace ours::mm {
namespace {
    auto dump_present_sections(MemSection *section, usize level) -> void {
        char const *format_auxiliary_spaces[] = {"      ", "    ", "  ", ""};
        for (usize i = 0; i < MemoryModel::kEntriesPerSection; ++i) {
            if (!section[i].is_state_present()) {
                continue;
            }

            log::debug("{}L{}[{}]: {:X}", format_auxiliary_spaces[level + 1], level, i,
                       usize(section));
            if (section[i].is_termianl()) {
                auto submap = section[i].memmap().data();
                log::debug("{}F: {:X}", format_auxiliary_spaces[0], usize(submap));
            } else {
                auto submap = section[i].secmap().data();
                dump_present_sections(submap, level - 1);
            }
        }
    }
} // namespace

class MemoryModel::Impl {
    using Self = Impl;

  public:
    template <typename T, typename... Args>
    FORCE_INLINE constexpr static auto allocate(usize n, usize a, Gaf gaf, NnId nid, Args &&...args)
        -> T *;

    template <typename T>
    FORCE_INLINE constexpr static auto dump(usize n, usize a, Gaf gaf, NnId nid) -> T *;
};

template <typename T, typename... Args>
FORCE_INLINE constexpr auto MemoryModel::Impl::allocate(usize n, usize a, Gaf gaf, NnId nid,
                                                        Args &&...args) -> T * {
    T *object;
    if (pmm_enabled()) {
        object = new (kGafKernel, nid) T[n](ustl::forward<Args>(args)...);
    } else {
        object = EarlyMem::allocate<T>(n, a, nid);
        if (!object) {
            panic("Failed to allocate {} map",
                  ustl::traits::IsSameV<T, PmFrame> ? "frame" : "section");
        }
    }

    // log::debug("{} map allocated at 0x{:8X}", ustl::traits::IsSameV<T, PmFrame> ? "Frame" : "Section", usize(object));
    return object;
}

auto MemoryModel::dump() -> void {
    log::debug("Section mapping information");
    dump_present_sections(m_root_section, kTopLevel);
}

INIT_CODE
auto MemoryModel::init(bool map_init) -> Status {
    auto root_section = Impl::allocate<MemSection>(kEntriesPerSection, PAGE_SIZE, kGafKernel, 0);
    if (!root_section) {
        panic("Failed to allocate memory for root section");
    }

    auto initialization_status = Status::Ok;
    PmNode::for_each_nid(PmNode::State::Possible, [&](NnId const nid) {
        if (initialization_status != Status::Ok) {
            return;
        }
        auto const visit_status = EarlyMem::visit_usable(
            EarlyMem::min_address(), EarlyMem::max_address(), nid,
            [this, &initialization_status, nid](bootmem::MemoryRegion const region) {
                initialization_status = add_range(region.range.base, region.range.end(), nid);
                return initialization_status == Status::Ok;
            });
        if (visit_status != Status::Ok && initialization_status == Status::Ok) {
            initialization_status = visit_status;
        }
    });
    if (initialization_status != Status::Ok) {
        return initialization_status;
    }

    if (map_init) {
        init_memmap();
    }
    return Status::Ok;
}

INIT_CODE
auto MemoryModel::init_memmap_reserved(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NnId nid)
    -> void {
    for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
        if (!exist(pfn)) {
            continue;
        }

        auto frame = pfn_to_frame(pfn);
        frame->init(ztype, pfn_to_secnum(pfn), nid);
        frame->mark_reserved();
    }
}

INIT_CODE
auto MemoryModel::init_memmap_range(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NnId nid) -> void {
    for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
        auto frame = pfn_to_frame(pfn);
        DEBUG_ASSERT(frame, "");

        frame->init(ztype, pfn_to_secnum(pfn), nid);
    }
}

INIT_CODE
auto MemoryModel::init_memmap_zone(ZoneType zone, Pfn zone_start, Pfn zone_end, Pfn start, Pfn end,
                                   Pfn hole_start, NnId nid) -> Pfn {
    start = std::clamp(start, zone_start, zone_end);
    end = std::clamp(end, zone_start, zone_end);

    if (start >= end) {
        return start;
    }
    init_memmap_range(start, end, zone, nid);

    if (hole_start < start) {
        init_memmap_reserved(hole_start, start, zone, nid);
    }
    return end;
}

INIT_CODE
auto MemoryModel::init_memmap() -> void {
    Pfn hole_start_pfn = 0, start_pfn = 0, end_pfn = 0;
    [[maybe_unused]]
    auto const status = EarlyMem::visit_usable(
        EarlyMem::min_address(), EarlyMem::max_address(), bootmem::kAnyNode,
        [this, &hole_start_pfn, &start_pfn, &end_pfn](bootmem::MemoryRegion const region) {
            auto const node = PmNode::node(region.node);
            DEBUG_ASSERT(node != nullptr, "");

            start_pfn = phys_to_pfn(region.range.base);
            end_pfn = phys_to_pfn(region.range.end());
            for (auto zone: node->zone_queues()->get_queue(ZoneQueues::LocalContiguous)) {
                start_pfn = init_memmap_zone(zone->zone_type(), zone->start_pfn(), zone->end_pfn(),
                                             start_pfn, end_pfn, hole_start_pfn, region.node);
                hole_start_pfn = end_pfn;
            }
            return true;
        });
    DEBUG_ASSERT(status == Status::Ok, "Failed to enumerate early memory");

    // A gift for zero number of node.
    end_pfn = ustl::mm::align_up(end_pfn, kFramesPerLevel[0]);
    if (hole_start_pfn < end_pfn) {
        init_memmap_reserved(hole_start_pfn, end_pfn, ZoneType::Normal, 0);
    }
}

/// Initializes the index the |secnum| needed.
auto MemoryModel::init_index(usize level, SecNum secnum, NnId nid)
    -> ustl::Result<MemSection *, Status> {
    auto section = m_root_section;

    // Establish all of interminal indexies.
    for (auto i = static_cast<usize>(kTopLevel); i > level; --i) {
        auto const index = secnum_to_index(i, secnum);
        section = ustl::mem::address_of(section[index]);

        if (section->is_state_present()) {
            // There is an available entry to next level. Let us to observe next level directly.
            section = section->secmap().data();
        } else {
            // No sub section map, need we to create a new.
            auto submap =
                Impl::allocate<MemSection>(kEntriesPerSection, PAGE_SIZE, kGafKernel, nid);
            if (!submap) {
                return ustl::err(Status::OutOfMem);
            }

            section->set_secmap(submap);
            section->set_level(i);
            section->set_state_present();
            section->mark_interminal();
            section = submap;
        }
    }

    // Because of may existing discrete commits to multiple ranges in a same leaf section, we
    // have to determine whether the leaf has been marked with terminal.
    //
    // There are a part of potential optimization points possibly:
    //   1) Normalizing the ranges in same section, make them was commited at once to avoid
    //      re-commit.
    section = ustl::mem::address_of(section[secnum_to_index(level, secnum)]);
    if (!section->is_termianl()) {
        section->set_level(level);
        section->mark_terminal();
    }
    return ustl::ok(section);
}

auto MemoryModel::activate_section(MemSection *section, NnId nid) -> ustl::Result<MemMap, Status> {
    DEBUG_ASSERT(section, "");
    if (section->is_state_present()) {
        return ustl::ok(section->memmap());
    }

    auto const nr_frames = kFramesPerLevel[section->level()];
    auto memmap = Impl::allocate<PmFrame>(nr_frames, PAGE_SIZE, kGafKernel, nid);
    if (!memmap) {
        return ustl::err(Status::OutOfMem);
    }

    section->set_memmap(memmap);
    section->set_state_present();
    return ustl::ok(MemMap(memmap, nr_frames));
}

template <typename F>
auto MemoryModel::descend(SecNum begin, SecNum end, F &&f) -> Status {
    for (isize level = kMappingLevel - 1; level >= 0; --level) {
        for (auto this_end = begin + kLeavesPerLevel[level]; this_end <= end;
             this_end += kLeavesPerLevel[level]) {
            auto status = ustl::function::invoke(ustl::forward<F>(f), level, begin);
            if (Status::Ok != status) {
                return status;
            }
            begin = this_end;
        }

        if (begin == end) {
            break;
        }
    }

    return Status::Ok;
}

auto MemoryModel::add_range(PhysAddr start, PhysAddr end, NnId nid) -> Status {
    auto process_range = [this, nid](usize level, SecNum sec) -> Status {
        auto result = init_index(level, sec, nid);
        if (result.is_err()) {
            return result.unwrap_err();
        }
        auto section = result.unwrap();
        if (auto activated = activate_section(section, nid); activated.is_err()) {
            // FIXME(SmallHuaZi): We should mark this section as absent. Rather than directly return.
            return activated.unwrap_err();
        }

        return Status::Ok;
    };

    auto const secnum_beg = phys_to_secnum(start);
    auto const secnum_end = phys_to_secnum(end) + 1;
    return descend(secnum_beg, secnum_end, process_range);
}

auto MemoryModel::mark_section(SecNum begin, SecNum end, MemSection::State state) -> void {
    while (begin != end) {
        auto section = secnum_to_section(begin);
        section->set_state(state);
        begin += kLeavesPerLevel[section->level()];
    }
}

} // namespace ours::mm
