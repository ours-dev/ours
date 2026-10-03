/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <elfload/program.hpp>

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

    /// The default mapping granule of one byte, for check_program(): a
    /// program the kernel maps as one run (kelfload::Loader::map()) may have
    /// two segments share a page harmlessly. segment_pages() is for a mapping
    /// by segment, where it is not harmless, and plans at page granularity.
    using Policy = eftl::elf::KernelElfLoadPolicy<Traits, Segments, MACHINE>;
    using PagePolicy = eftl::elf::KernelElfLoadPolicy<Traits, Segments, MACHINE, MAPPING_PAGE_SIZE>;

    constexpr auto page_down(u64 const value) -> u64 {
        return value & ~(MAPPING_PAGE_SIZE - 1);
    }

    constexpr auto page_up(u64 const value) -> u64 {
        return page_down(value + MAPPING_PAGE_SIZE - 1);
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

auto segment_pages(Diagnostics const &diagnostics, ustl::views::Span<u8 const> const file,
                   SegmentPagesList &out) -> Status {
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

    // The plan's regions are exact to the byte; a mapping takes the whole
    // pages around each. make_plan() has already refused two that share a
    // page, and the capacity is the plan's own, so neither the subtraction
    // nor the push can fail.
    auto const first_page = page_down(plan->image_base());
    for (auto const &region: plan->regions()) {
        auto const start = page_down(region.virtual_start);
        auto const has = [&region](eftl::elf::SegPerms const perm) {
            return (region.permissions & perm) == perm;
        };
        static_cast<void>(out.push_back({
            .offset = start - first_page,
            .size = page_up(region.virtual_end) - start,
            .read = has(eftl::elf::SegPerms::Readable),
            .write = has(eftl::elf::SegPerms::Writable),
            .execute = has(eftl::elf::SegPerms::Executable),
        }));
    }
    return Status::Ok;
}
} // namespace elfload
