/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/bootfs.hpp>
#include <launch/plan.hpp>

#include <limits>

#include <bootfs/format.hpp>
#include <bootfs/view.hpp>
#include <ours/rights.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <ustl/mem/align.hpp>

namespace launch {
namespace {
    using ours::Status;
    using ours::Uandle;

    /// A step that failed, said through |diagnostics|, and its status handed
    /// back for the caller to return: the way start() reports its own steps.
    auto failed(elfload::Diagnostics const &diagnostics, std::string_view const step,
                Status const status) -> Status {
        diagnostics({.what = step, .name = ours::to_string(status)});
        return status;
    }

    /// The BootFS in |file| as bootfs::View checks it: the header, the
    /// directory and every file's extent against the bytes actually there.
    auto view_of(std::span<u8 const> const file) -> bootfs::View::Result {
        return bootfs::View::from({file.data(), file.size()});
    }
} // namespace

auto map_bootfs(Uandle const bootfs, Uandle const vma, elfload::Diagnostics const diagnostics)
    -> ustl::Result<std::span<u8 const>, Status> {
    // The header through the VMO first: it alone says how long the BootFS
    // is, and so how much to map. Nothing in it is trusted yet; View checks
    // it against the mapping.
    bootfs::Header header{};
    if (auto const status = ours::vmo_read(bootfs, &header, 0, sizeof(header));
        status != Status::Ok) {
        return ustl::err(failed(diagnostics, "reading the BootFS header", status));
    }
    // Whole pages, and in a usize: a length that wraps either way is refused
    // here rather than mapped short.
    auto const mapped = ustl::round_up(header.total_size, u64{PAGE_SIZE});
    if (mapped < header.total_size || mapped > std::numeric_limits<usize>::max()) {
        return ustl::err(failed(diagnostics, "sizing the BootFS mapping", Status::OutOfRange));
    }
    ours::VirtAddr address{};
    if (auto const status = ours::vma_map(vma, ours::VmOptions::PermRead, 0, bootfs, 0,
                                          static_cast<usize>(mapped), &address);
        status != Status::Ok) {
        return ustl::err(failed(diagnostics, "mapping the BootFS", status));
    }
    return ustl::ok(std::span<u8 const>{reinterpret_cast<u8 const *>(address),
                                        static_cast<usize>(header.total_size)});
}

auto make_bootfs_launcher(Uandle const bootfs, Uandle const vmex, Uandle const vdso,
                          Uandle const vma, elfload::Diagnostics const diagnostics)
    -> ustl::Result<BootfsLauncher, Status> {
    auto const mapped = map_bootfs(bootfs, vma, diagnostics);
    if (mapped.is_err()) {
        return ustl::err(mapped.unwrap_err());
    }
    auto const file = mapped.unwrap();

    // Every failure from here takes the mapping down again before it is
    // reported, so a caller that gets an error is left with nothing of the
    // launcher in its address space. The length rounds up to the pages
    // map_bootfs() mapped.
    auto const unmapped = [&](std::string_view const step, Status const status) -> Status {
        auto _ =
            ours::vma_unmap(vma, ours::VirtAddr{reinterpret_cast<usize>(file.data())}, file.size());
        return failed(diagnostics, step, status);
    };

    // From here the BootFS is read in place, through the mapping.
    auto const view = view_of(file);
    if (view.is_err()) {
        return ustl::err(unmapped("checking the BootFS", view.unwrap_err()));
    }
    auto const found = view.unwrap().find("vdso");
    if (found.is_err()) {
        return ustl::err(unmapped("finding the vdso in the BootFS", found.unwrap_err()));
    }
    if (found.unwrap().is_none()) {
        return ustl::err(unmapped("finding the vdso in the BootFS", Status::NotFound));
    }
    auto const vdso_file = view.unwrap().file_data(found.unwrap().value());

    // Then the executable copy. vmo_replace_as_executable consumes the
    // handle it is given whatever it answers, so it is given a duplicate:
    // the caller keeps |bootfs|.
    Uandle copy = ours::Uandle::Invalid;
    if (auto const status = ours::handle_duplicate(bootfs, ours::Rights::SameRights, &copy);
        status != Status::Ok) {
        return ustl::err(unmapped("duplicating the BootFS", status));
    }
    Uandle executable = ours::Uandle::Invalid;
    if (auto const status = ours::vmo_replace_as_executable(copy, vmex, &executable);
        status != Status::Ok) {
        return ustl::err(unmapped("making the BootFS executable", status));
    }

    return ustl::ok(BootfsLauncher{
        .executable = executable,
        .vdso = vdso,
        .vma = vma,
        .bootfs_file = file,
        .vdso_file = {vdso_file.data(), vdso_file.size()},
    });
}

auto launch_from_bootfs(BootfsLauncher const &launcher, Uandle const job,
                        std::string_view const name, Uandle const bootstrap,
                        elfload::Diagnostics const diagnostics) -> ustl::Result<Started, Status> {
    // Checked again rather than kept from make_bootfs_launcher(): a few
    // words of header against the mapping, and the launcher stays a plain
    // struct of handles and spans.
    auto const view = view_of(launcher.bootfs_file);
    auto status = Status::Ok;
    auto entry = bootfs::Entry{};
    if (view.is_err()) {
        status = failed(diagnostics, "checking the BootFS", view.unwrap_err());
    } else if (auto const found = view.unwrap().find(name); found.is_err()) {
        status = failed(diagnostics, "finding the program in the BootFS", found.unwrap_err());
    } else if (found.unwrap().is_none()) {
        diagnostics({.what = "no such program in the BootFS", .name = name});
        status = Status::NotFound;
    } else {
        entry = found.unwrap().value();
    }
    if (status != Status::Ok) {
        // start() would have consumed it; a launch that never got that far
        // consumes it here, so that the caller's rule is the same either way.
        auto _ = ours::handle_close(bootstrap);
        return ustl::err(status);
    }

    // The program's file VMO: a slice of the executable BootFS, so that it
    // carries Execute, which start() asks of the file it reads, and its code
    // can be mapped executable as it is. View has checked that the slice
    // lies inside the BootFS. start() reads the program through the slice,
    // not through this process's mapping of the BootFS: one VMO feeds both
    // the plan and the mappings, so they cannot be of two different files.
    Uandle file = ours::Uandle::Invalid;
    if (auto const sliced =
            ours::vmo_create_child(launcher.executable, ours::VmoChildOptions::Slice,
                                   entry.data_offset, entry.data_size, &file);
        sliced != Status::Ok) {
        auto _ = ours::handle_close(bootstrap);
        return ustl::err(failed(diagnostics, "slicing the program out of the BootFS", sliced));
    }
    auto started = start({
        .job = job,
        .name = name,
        .file = file,
        .vma = launcher.vma,
        .vdso = launcher.vdso,
        .vdso_file = launcher.vdso_file,
        .clock_page = launcher.clock_page,
        .bootstrap = bootstrap,
        .diagnostics = diagnostics,
    });
    // The mappings of the program's code hold the file's VMO; this handle is
    // not needed for them.
    auto _ = ours::handle_close(file);
    return started;
}
} // namespace launch
