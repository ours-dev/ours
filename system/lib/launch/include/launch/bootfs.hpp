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
/// The shell and init both start their programs this way (U7).
///
/// Three things, each because of how start() takes a program:
///   - the BootFS made executable, which every program's file VMO is a slice
///     of, since start() maps a program's code from its file as it is;
///   - the BootFS mapped read-only into this process, from which start()
///     reads a program's headers and dynamic tables;
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

    /// The whole BootFS, mapped read-only into this process for good: there
    /// is no vma_unmap to take it out again.
    std::span<u8 const> bootfs_file;

    /// The vDSO's file, a part of |bootfs_file|.
    std::span<u8 const> vdso_file;
};

/// Makes the launcher out of |bootfs| (which must grant Read, Map and
/// Duplicate), |vmex| (the VMEX resource), |vdso| (the vDSO's VMO) and |vma|
/// (this process's root region, where the BootFS is mapped). Consumes none of
/// them.
///
/// The steps: read the BootFS header, map the whole BootFS read-only, check
/// it (bootfs::View::from()) and find `vdso` in it, then duplicate the BootFS
/// handle and make the copy executable. On failure nothing it made is left
/// behind, except the mapping (no vma_unmap). The status is that of the step
/// that failed, which is also reported to |diagnostics| as an Error, |what|
/// naming the step: InvArgs or OutOfRange for a BootFS that View refuses,
/// NotFound when it has no `vdso`, or the system call's own answer.
[[nodiscard]]
auto make_bootfs_launcher(ours::Uandle bootfs, ours::Uandle vmex, ours::Uandle vdso,
                          ours::Uandle vma, elfload::Diagnostics diagnostics = {})
    -> ustl::Result<BootfsLauncher, ours::Status>;

/// Starts the BootFS file |name| in a new process under |job|, |bootstrap| as
/// its one handle: consumed whatever the answer, as start() consumes it. The
/// program's file VMO is a slice of the executable BootFS, closed again once
/// the process holds its mappings.
///
/// NotFound when the BootFS has no such file; otherwise start()'s answer.
/// Every failure is reported to |diagnostics|, as start() reports its own.
[[nodiscard]]
auto launch_from_bootfs(BootfsLauncher const &launcher, ours::Uandle job, std::string_view name,
                        ours::Uandle bootstrap, elfload::Diagnostics diagnostics = {})
    -> ustl::Result<Started, ours::Status>;
} // namespace launch

#endif // #ifndef LAUNCH_BOOTFS_HPP
