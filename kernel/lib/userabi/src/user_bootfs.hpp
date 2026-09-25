/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USERABI_SRC_USER_BOOTFS_HPP
#define USERABI_SRC_USER_BOOTFS_HPP 1

#include <string_view>

#include <ours/object/handle.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/phys/kernel_package.hpp>
#include <ours/status.hpp>

namespace userabi {
/// The rights user space holds the BootFS VMO with: it may read it, map it,
/// ask its size, and pass it on -- and nothing else.
///
/// No Write, because every program that is handed the BootFS reads the same
/// bytes, and one that could change them would be changing its successors'
/// programs. No Execute either: a program that wants to run a file out of
/// it trades the handle for an executable one through the vmex resource
/// (design D3), and that trade drops Write, which is why the two never meet
/// on one handle.
inline constexpr auto USER_BOOTFS_RIGHTS =
    ours::Rights::Basic | ours::Rights::Read | ours::Rights::Map | ours::Rights::GetProperty;

/// Builds the BootFS that user space is given: a fresh VMO holding only the
/// files of |package| that its whitelist, `config/user_bootfs`, names.
///
/// The kernel package and the files user space may see are one BootFS in the
/// image, and handing the package out whole would hand out the kernel's own
/// ELF with it -- which, under KASLR, is the layout less its base. So the
/// VMO is rebuilt, not shared: header, entry table and name table are
/// written anew for the files that pass, and the bytes of everything else
/// never reach it. Hiding entries by name while giving out the original
/// bytes would leave them one vmo_read at the right offset away.
///
/// What passes is decided twice, in this order:
///
///  - the whitelist, which the build generates from GN metadata on the
///    targets that produce the files (//system/user_executable.gni). A file
///    nobody marked stays in the kernel package; that is the default.
///  - a blacklist behind it: the directory named by |kernel_name| (the boot
///    option `kernel.name`) and any file with one of KernelPackage's fixed
///    names -- the kernel image and its code patches, wherever they sit --
///    are refused even when listed, with an error at boot. The whitelist is
///    a build product, and a mistake in it must fail towards giving less.
///
/// Every refusal is logged, and none of them stops the rest: a BootFS with
/// one file missing boots into a program that says what it could not find,
/// which is easier to follow than no user space at all.
///
/// File data starts on a page boundary and is zero-padded to the next, so
/// no two files share a page and a mapping of one reveals nothing of
/// another. The host-side bootfs::Builder still aligns to 8; the kernel can
/// be right from the start because it lays this one out itself.
///
/// |out| is left empty on failure. On success |rights_out| is the rights a
/// handle to it should carry: USER_BOOTFS_RIGHTS, within what the VMO was
/// created with.
auto make_user_bootfs(ours::phys::KernelPackage const &package, std::string_view kernel_name,
                      ours::object::KernelHandle<ours::object::VmoDispatcher> &out,
                      ours::Rights &rights_out) -> ours::Status;
} // namespace userabi

#endif // #ifndef USERABI_SRC_USER_BOOTFS_HPP
