/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <elfload/program.hpp>

#include <eftl/elf/binary.hpp>
#include <eftl/elf/load_policy.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/traits.hpp>
#include <logz4/log.hpp>
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

    /// Well above what lld produces -- read-only, code, RELRO and data make
    /// four -- and the same bound as //kernel/phys:elf_image. A fixed vector
    /// because the plan lives only for this call, and a header claiming a
    /// hundred segments is refused as CapacityExceeded without allocating
    /// room for them first.
    constexpr auto MAX_LOAD_SEGMENTS = usize{12};

    template <typename T>
    using Segments = ustl::collections::StaticVec<T, MAX_LOAD_SEGMENTS>;

    /// The default mapping granule of one byte: Loader::map() lays the whole
    /// image out as one run, so two segments sharing a page is harmless
    /// today. It stops being harmless when each segment gets its own
    /// permissions (docs/repairs/260919/elfload.md), and PAGE_SIZE goes here
    /// with that change.
    using Policy = eftl::elf::KernelElfLoadPolicy<Traits, Segments, MACHINE>;
} // namespace

auto check_program(ustl::views::Span<u8 const> const file) -> Status {
    auto const parsed = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (parsed.is_err()) {
        log::error("elfload: not an ELF this kernel can read: {}",
                   eftl::elf::to_string(parsed.unwrap_err()));
        return Status::InvArgs;
    }

    // BasicImage::from() insists on an executable entry only for ET_EXEC --
    // for ET_DYN it assumes a loadable module, whose entry lives in its own
    // descriptor. A PIE program is ET_DYN too, so that half is asked again
    // below of the plan, for both types.
    auto const image = eftl::elf::BasicImage<Traits>::from(parsed.value());
    if (image.is_none()) {
        log::error("elfload: not a loadable image");
        return Status::InvArgs;
    }

    // Type, machine, overlap, and an entry inside some PT_LOAD.
    auto const plan = Policy::make_plan(image.value());
    if (plan.is_err()) {
        log::error("elfload: cannot be laid out: {}", eftl::elf::to_string(plan.unwrap_err()));
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
    log::error("elfload: the entry point {:X} is not in an executable segment", entry);
    return Status::InvArgs;
}
} // namespace elfload
