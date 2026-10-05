/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_BOOTFS_HPP
#define LAUNCH_BOOTFS_HPP 1

#include <launch/launch.hpp>

#include <span>
#include <string_view>

#include <elfload/diagnostics.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

namespace launch {
/// Programs started by name from a BootFS: what start() needs besides the
/// program, made once by make_bootfs_launcher() and kept for every launch.
/// Only the two processes that hold the raw BootFS and VMEX start programs
/// this way: userboot, which starts what userboot.next names, and the dbg_1
/// shell, which has no root to ask. On dev nothing else does: the root hands
/// each file out as its own VMO from /boot, and fuchsia.process.Launcher
/// takes that VMO, not a name.
///
/// Four things, each because of how start() takes a program:
///   - the BootFS made executable, which every program's file VMO is a slice
///     of, since start() maps a program's file with Execute to read it, and
///     maps its code from it as it is;
///   - this process's root region, where start() maps that file while it
///     reads it;
///   - the BootFS mapped read-only into this process, where a program is
///     found by name;
///   - the vDSO's file, inside that mapping, which start() binds a program's
///     imports against.
///
/// Fuchsia does the first part in component_manager: it parses the BootFS
/// once and hands each file out as its own VMO, made executable with vmex
/// where it is a program (src/sys/component_manager/src/bootfs.rs,
/// create_vmo_from_bootfs()); a launcher then asks for a program by path
/// before process_builder starts it.
struct BootfsLauncher {
    /// The BootFS made executable with vmex. Owned: the caller closes it once
    /// it launches nothing more.
    ours::Uandle executable = ours::Uandle::Invalid;

    /// The vDSO's VMO (HandleType::VmoVdso), mapped into every process
    /// started. Not owned: the handle the caller passed in.
    ours::Uandle vdso = ours::Uandle::Invalid;

    /// This process's root region (HandleType::VmaRoot), where start() maps
    /// each program's file while it reads it. Not owned: the handle the
    /// caller passed in, kept here because the runtime hands VmaRoot out
    /// once (ort::take_startup_handle()), and every launch needs it.
    ours::Uandle vma = ours::Uandle::Invalid;

    /// The whole BootFS, mapped read-only into this process for as long as
    /// the launcher is used: every launch finds its program in it by name,
    /// and binds against |vdso_file| in it. Nothing unmaps it but the
    /// process's exit, or the caller with vma_unmap once it launches nothing
    /// more.
    std::span<u8 const> bootfs_file;

    /// The vDSO's file, a part of |bootfs_file|.
    std::span<u8 const> vdso_file;
};

/// Maps the whole BootFS |bootfs| read-only into |vma|, and answers the
/// mapping, |total_size| bytes long; it is the caller's from then on, to keep
/// or to take down with vma_unmap. Its header is read through the VMO first,
/// since it alone says how much to map; nothing in it is trusted, and
/// bootfs::View checks it against the mapping. For a program that reads the
/// BootFS without starting anything from it. Failures are reported to
/// |diagnostics| as make_bootfs_launcher()'s are.
[[nodiscard]]
auto map_bootfs(ours::Uandle bootfs, ours::Uandle vma, elfload::Diagnostics diagnostics = {})
    -> ustl::Result<std::span<u8 const>, ours::Status>;

/// Makes the launcher out of |bootfs| (which must grant Read, Map and
/// Duplicate), |vmex| (the VMEX resource), |vdso| (the vDSO's VMO) and |vma|
/// (this process's root region, where the BootFS is mapped, and each program
/// while start() reads it). Consumes none of them; the launcher keeps |vdso|
/// and |vma| without owning them, so both must stay open while it is used.
///
/// The steps: read the BootFS header, map the whole BootFS read-only, check
/// it (bootfs::View::from()) and find `vdso` in it, then duplicate the BootFS
/// handle and make the copy executable. On failure nothing it made is left
/// behind: the mapping is taken down again. The status is that of the step
/// that failed, which is also reported to |diagnostics| as an Error, |what|
/// naming the step: InvArgs or OutOfRange for a BootFS that View refuses,
/// NotFound when it has no `vdso`, or the system call's own answer.
[[nodiscard]]
auto make_bootfs_launcher(ours::Uandle bootfs, ours::Uandle vmex, ours::Uandle vdso,
                          ours::Uandle vma, elfload::Diagnostics diagnostics = {})
    -> ustl::Result<BootfsLauncher, ours::Status>;

/// Starts the BootFS file |name| in a new process under |job|, |bootstrap| as
/// its one handle: consumed whatever the answer, as start() consumes it. The
/// program's file VMO is a slice of the executable BootFS, which start()
/// reads through a mapping of its own and maps the code from; the slice is
/// closed again once the process holds its mappings.
///
/// NotFound when the BootFS has no such file; otherwise start()'s answer.
/// Every failure is reported to |diagnostics|, as start() reports its own.
[[nodiscard]]
auto launch_from_bootfs(BootfsLauncher const &launcher, ours::Uandle job, std::string_view name,
                        ours::Uandle bootstrap, elfload::Diagnostics diagnostics = {})
    -> ustl::Result<Started, ours::Status>;
} // namespace launch

#endif // #ifndef LAUNCH_BOOTFS_HPP
