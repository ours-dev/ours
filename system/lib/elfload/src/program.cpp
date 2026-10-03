/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <elfload/program.hpp>

#include <algorithm>
#include <limits>

#include <eftl/elf/binary.hpp>
#include <eftl/elf/load_policy.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/traits.hpp>
#include <ustl/collections/static_vec.hpp>

namespace elfload {
using namespace ours;

namespace {
    using Traits = eftl::elf::TraitsNative;

#if defined(__x86_64__)
    constexpr auto MACHINE = eftl::elf::Arch::X86_64;
#else
#error "elfload has no ELF machine for this target; its binder is x86-64 only as well"
#endif

    /// MAX_LOAD_SEGMENTS (<elfload/program.hpp>) is the same bound as
    /// //kernel/phys:elf_image. A fixed vector because the plan lives only for
    /// this call, and a header claiming a hundred segments is refused as
    /// CapacityExceeded without allocating room for them first.
    template <typename T>
    using Segments = ustl::collections::StaticVec<T, MAX_LOAD_SEGMENTS>;

    /// The default mapping granule of one byte, for check_program(), which
    /// asks what makes a file a program and is asked by the kernel and the
    /// launcher alike. Whether its segments can each have pages of their own
    /// is the question of mapping it, which each of them asks when it maps:
    /// mapping_pages() here, at page granularity, and the launcher's own Plan.
    using Policy = eftl::elf::KernelElfLoadPolicy<Traits, Segments, MACHINE>;
    using PagePolicy = eftl::elf::KernelElfLoadPolicy<Traits, Segments, MACHINE, MAPPING_PAGE_SIZE>;

    constexpr auto page_down(u64 const value) -> u64 {
        return value & ~(MAPPING_PAGE_SIZE - 1);
    }

    constexpr auto page_up(u64 const value) -> u64 {
        return page_down(value + MAPPING_PAGE_SIZE - 1);
    }

    constexpr auto has(eftl::elf::LoadRegion const &region, eftl::elf::SegPerms const perm)
        -> bool {
        return (region.permissions & perm) == perm;
    }

    /// Whether |relro| is a prefix of |region|: the whole of a writable
    /// segment's start, which is where a linker puts RELRO.
    constexpr auto is_relro_of(eftl::elf::RelroInterval const &relro,
                               eftl::elf::LoadRegion const &region) -> bool {
        return has(region, eftl::elf::SegPerms::Writable) &&
               relro.virtual_start == region.virtual_start &&
               relro.virtual_end <= region.virtual_end;
    }
} // namespace

auto check_program(Diagnostics const &diagnostics, ustl::views::Span<u8 const> const file)
    -> Status {
    auto const parsed = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (parsed.is_err()) {
        diagnostics({.what = "not an ELF this system can read",
                     .name = eftl::elf::to_string(parsed.unwrap_err())});
        return Status::InvArgs;
    }

    // BasicImage::from() insists on an executable entry only for ET_EXEC --
    // for ET_DYN it assumes a loadable module, whose entry lives in its own
    // descriptor. A PIE program is ET_DYN too, so that half is asked again
    // below of the plan, for both types.
    auto const image = eftl::elf::BasicImage<Traits>::from(parsed.value());
    if (image.is_none()) {
        diagnostics({.what = "not a loadable image"});
        return Status::InvArgs;
    }

    // Type, machine, overlap, and an entry inside some PT_LOAD.
    auto const plan = Policy::make_plan(image.value());
    if (plan.is_err()) {
        diagnostics(
            {.what = "cannot be laid out", .name = eftl::elf::to_string(plan.unwrap_err())});
        return Status::InvArgs;
    }

    // Inside a PT_LOAD is not enough: the first one of an lld image holds the
    // ELF header and read-only data, and a PIE whose e_entry is 0 -- which
    // is what a shared object such as the vDSO has -- points straight into
    // it. Started there, the thread would execute the header as code.
    auto const entry = plan->entrypoint();
    for (auto const &region: plan->regions()) {
        if (entry < region.virtual_start || entry >= region.virtual_end) {
            continue;
        }
        if ((region.permissions & eftl::elf::SegPerms::Executable) ==
            eftl::elf::SegPerms::Executable) {
            return Status::Ok;
        }
        break;
    }
    diagnostics({.what = "the entry point is not in an executable segment",
                 .value = ustl::some(static_cast<u64>(entry))});
    return Status::InvArgs;
}

auto mapping_pages(Diagnostics const &diagnostics, ustl::views::Span<u8 const> const file,
                   MappingPagesList &out) -> Status {
    using eftl::elf::SegPerms;

    out.clear();
    auto const parsed = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (parsed.is_err()) {
        diagnostics({.what = "not an ELF this system can read",
                     .name = eftl::elf::to_string(parsed.unwrap_err())});
        return Status::InvArgs;
    }
    auto const image = eftl::elf::BasicImage<Traits>::from(parsed.value());
    if (image.is_none()) {
        diagnostics({.what = "not a loadable image"});
        return Status::InvArgs;
    }
    auto const plan = PagePolicy::make_plan(image.value());
    if (plan.is_err()) {
        diagnostics({.what = "cannot be mapped by segment",
                     .name = eftl::elf::to_string(plan.unwrap_err())});
        return Status::InvArgs;
    }

    // Pass 1 only judges, as the launcher's Plan::make() does, so that a
    // refused image leaves |out| empty rather than holding the mappings of
    // the segments before the one refused.
    //
    // The plan's regions are exact to the byte, in program-header order, and
    // eftl has checked that none ends past the top of the address space.
    // Rounding an end up to a page is a second place to wrap, checked here
    // so that pass 2 can round without a check.
    auto previous_end = u64{0};
    for (auto const &region: plan->regions()) {
        auto const at = ustl::some(region.virtual_start);
        // W^X is the system's rule. Bits that are not access bits are ignored,
        // as the launcher ignores them: they carry nothing a mapping acts on.
        auto const access =
            region.permissions & (SegPerms::Readable | SegPerms::Writable | SegPerms::Executable);
        if (access == SegPerms::None) {
            diagnostics(
                {.what = "a segment is neither readable, writable nor executable", .value = at});
            return Status::InvArgs;
        }
        if (has(region, SegPerms::Writable) && has(region, SegPerms::Executable)) {
            diagnostics({.what = "a segment is both writable and executable", .value = at});
            return Status::InvArgs;
        }
        if (region.virtual_end > std::numeric_limits<u64>::max() - (MAPPING_PAGE_SIZE - 1)) {
            diagnostics(
                {.what = "a segment ends too near the top of the address space", .value = at});
            return Status::InvArgs;
        }
        // A page has one protection, so two segments cannot share one. eftl
        // has refused that already, but on pages counted from the image's
        // lowest address, which is not a page boundary unless the image says
        // so; this counts them where they are mapped. Out of order is refused
        // with it, as the launcher refuses it.
        if (page_down(region.virtual_start) < previous_end) {
            diagnostics({.what = "a segment shares a page with the one before it, or comes "
                                 "before it",
                         .value = at});
            return Status::InvArgs;
        }
        previous_end = page_up(region.virtual_end);
    }
    // RELRO is what a linker puts at the start of a writable segment, so each
    // range belongs to the writable segment that starts where it does. One
    // anywhere else -- in a segment that is not writable, or partway into
    // one -- describes a layout no mapping by whole pages of a segment can
    // honour.
    for (auto const &relro: plan->relro_intervals()) {
        auto placed = false;
        for (auto const &region: plan->regions()) {
            placed = placed || is_relro_of(relro, region);
        }
        if (!placed) {
            diagnostics({.what = "PT_GNU_RELRO is not the start of a writable segment",
                         .value = ustl::some(relro.virtual_start)});
            return Status::InvArgs;
        }
    }

    // Pass 2 writes the mappings. A segment gives at most two and the
    // capacity is twice the plan's own, so no push can fail; every
    // subtraction is of an address from a larger one.
    auto const first_page = page_down(plan->image_base());
    for (auto const &region: plan->regions()) {
        auto const first = page_down(region.virtual_start);
        auto const end = page_up(region.virtual_end);

        // The end of the pages that are wholly RELRO, |first| if none is. A
        // page RELRO only partly fills also holds data that is written to,
        // so the end rounds down and that page stays writable. RELRO is no
        // longer than its segment (pass 1), so this is never past |end|.
        // Two ranges at the start of one segment make one, the longer.
        auto relro_end = first;
        for (auto const &relro: plan->relro_intervals()) {
            if (is_relro_of(relro, region)) {
                relro_end = std::max(relro_end, page_down(relro.virtual_end));
            }
        }

        if (relro_end > first) {
            auto _ = out.push_back({
                .offset = first - first_page,
                .size = relro_end - first,
                .write = false,
                .execute = false,
                .relro = true,
            });
        }
        if (end > relro_end) {
            auto _ = out.push_back({
                .offset = relro_end - first_page,
                .size = end - relro_end,
                .write = has(region, SegPerms::Writable),
                .execute = has(region, SegPerms::Executable),
                .relro = false,
            });
        }
    }
    return Status::Ok;
}
} // namespace elfload
