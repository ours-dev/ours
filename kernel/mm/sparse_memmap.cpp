#include <ours/mm/early_mem.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/sparse_memmap.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/panic.hpp>

#include <algorithm>

#include <heap/new.hpp>
#include <logz4/log.hpp>
namespace ours::mm::priv {
class SparseMemMap::Impl {
    using Self = Impl;

  public:
    template <typename T, typename... Args>
    FORCE_INLINE
    constexpr static auto allocate(usize n, usize a, Gaf gaf, NodeId nid, Args &&...args) -> T *;

    template <typename T>
    FORCE_INLINE
    constexpr static auto dump(usize n, usize a, Gaf gaf, NodeId nid) -> T *;
};

template <typename T, typename... Args>
FORCE_INLINE
constexpr auto SparseMemMap::Impl::allocate(usize n, usize a, Gaf gaf, NodeId nid, Args &&...args)
    -> T * {
    T *object;
    // heap_enabled(), not pmm_enabled(): the branch below is `new`, so what it
    // needs is kmalloc, not the page allocator. These were one stage until the
    // buddy's arrival was given a stage of its own.
    if (heap_enabled()) {
        object = new (gaf, nid) T[n](ustl::forward<Args>(args)...);
    } else {
        object = EarlyMem::allocate<T>(n, a, nid, bootmem::MemoryType::KernelData);
        if (!object) {
            panic("Failed to allocate {} map",
                  ustl::traits::IsSameV<T, PmFrame> ? "frame" : "section");
        }

        // The boot allocator has no notion of Gaf and hands back whatever was
        // last in that memory, so Gaf::Zero has to be honoured here or a caller
        // that asks for it silently gets garbage on this path and clean memory
        // on the other -- a difference that only shows up as a wild pointer far
        // from the allocation.
        if (!!(gaf & Gaf::Zero)) {
            std::fill_n(reinterpret_cast<u8 *>(object), n * sizeof(T), u8(0));
        }
    }

    // log::debug("{} map allocated at 0x{:8X}", ustl::traits::IsSameV<T, PmFrame> ? "Frame" : "Section", usize(object));
    return object;
}

auto SparseMemMap::dump() -> void {
    log::debug("Section mapping information");
    auto const dump_sections = [](this auto const &self, MemSection *section, usize level) -> void {
        char const *format_auxiliary_spaces[] = {"      ", "    ", "  ", ""};
        for (usize i = 0; i < SparseMemMap::ENTRIES_PER_SECTION; ++i) {
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
                self(submap, level - 1);
            }
        }
    };
    dump_sections(m_root_section, TOP_LEVEL);
}

INIT_CODE
auto SparseMemMap::init() -> Status {
    // Zeroed, not merely allocated. A section table encodes "no entry here" as an
    // all-zero entry -- init_index() decides whether to descend or to build a new
    // level purely from is_state_present() -- so uninitialised memory reads as a
    // random mixture of present entries pointing at whatever the previous owner
    // left behind. GAF_KERNEL is DirectReclaim alone and does *not* imply
    // Gaf::Zero, so it has to be asked for.
    auto root_section =
        Impl::allocate<MemSection>(ENTRIES_PER_SECTION, PAGE_SIZE, GAF_KERNEL | Gaf::Zero, 0);
    if (!root_section) {
        panic("Failed to allocate memory for root section");
    }

    // Publishing it is what makes every later lookup work: priv_secnum_to_section()
    // starts its descent at m_root_section, and the base's add_range() pass is the
    // first caller.
    m_root_section = root_section;
    return Status::Ok;
}

/// Initializes the index the |secnum| needed.
auto SparseMemMap::init_index(usize level, SecNum secnum, NodeId nid)
    -> ustl::Result<MemSection *, Status> {
    auto section = m_root_section;

    // Establish all of interminal indexies.
    for (auto i = static_cast<usize>(TOP_LEVEL); i > level; --i) {
        auto const index = secnum_to_index(i, secnum);
        section = ustl::mem::address_of(section[index]);

        if (section->is_state_present()) {
            // There is an available entry to next level. Let us to observe next level directly.
            section = section->secmap().data();
        } else {
            // No sub section map, need we to create a new.
            auto submap =
                // Zeroed for the same reason as the root table; see init().
                Impl::allocate<MemSection>(ENTRIES_PER_SECTION, PAGE_SIZE, GAF_KERNEL | Gaf::Zero,
                                           nid);
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

auto SparseMemMap::activate_section(MemSection *section, NodeId nid)
    -> ustl::Result<FrameSpan, Status> {
    DEBUG_ASSERT(section, "");
    if (section->is_state_present()) {
        return ustl::ok(section->memmap());
    }

    // Zeroed. A descriptor's "nothing set yet" state is all-zero, and one field
    // in particular has to start that way: PmFrame::m_hook is an AutoUnlink
    // intrusive hook, whose unlinked representation is a null next pointer. The
    // buddy's insert_frame() refuses to link a node that looks already-linked,
    // so a hook full of leftover bytes takes the whole kernel down the first
    // time a frame is freed -- as an unreachable-path trap, with nothing on the
    // stack to say why.
    //
    // PmFrame::init() cannot be the place that fixes this: it is only called
    // for pfns that are really there, while every descriptor in the section --
    // holes included -- ends up on some list eventually. Zeroing the whole
    // allocation is also what makes init()'s "already initialised" assert mean
    // anything, since it reads a flag out of this memory.
    //
    // Allocating it raw would be safe only if every descriptor's own
    // initialisation opened by zeroing it, which pays the same cost, one
    // descriptor at a time.
    auto const nr_frames = FRAMES_PER_LEVEL[section->level()];
    auto memmap = Impl::allocate<PmFrame>(nr_frames, PAGE_SIZE, GAF_KERNEL | Gaf::Zero, nid);
    if (!memmap) {
        return ustl::err(Status::OutOfMem);
    }

    section->set_memmap(memmap);
    section->set_state_present();
    return ustl::ok(FrameSpan(memmap, nr_frames));
}

template <typename F>
auto SparseMemMap::descend(SecNum begin, SecNum end, F &&f) -> Status {
    for (isize level = MAPPING_LEVEL - 1; level >= 0; --level) {
        for (auto this_end = begin + LEAVES_PER_LEVEL[level]; this_end <= end;
             this_end += LEAVES_PER_LEVEL[level]) {
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

auto SparseMemMap::add_range(PhysAddr start, PhysAddr end, NodeId nid) -> Status {
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

auto SparseMemMap::mark_section(SecNum begin, SecNum end, MemSection::State state) -> void {
    while (begin != end) {
        auto section = secnum_to_section(begin);
        section->set_state(state);
        begin += LEAVES_PER_LEVEL[section->level()];
    }
}

} // namespace ours::mm::priv
