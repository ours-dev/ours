/// Copyright(C) 2024 smallhuazi
///
/// This program is free software; you can redistribute it and/or modify
/// it under the terms of the GNU General Public License as published
/// by the Free Software Foundation; either version 2 of the License, or
/// (at your option) any later version.
///
/// For additional information, please refer to the following website:
/// https://opensource.org/license/gpl-2-0
///
#ifndef OURS_SYSCALL_RESOURCE_HPP
#define OURS_SYSCALL_RESOURCE_HPP 1

namespace ours {
enum class RsrcKind {
    MmIo,
    Irq,
    IoPort,
    Root,
    System,
    NumKinds,
};

enum class RsrcFlag {

};

/// The slots of the System resource kind: a System resource of size 1 at one
/// of these bases is the capability the slot names, and nothing else is. The
/// values are Zircon's (ZX_RSRC_SYSTEM_*_BASE in
/// zircon/system/public/zircon/syscalls/resource.h), so that code ported from
/// Zircon asks for the same slot. Base 0, Zircon's hypervisor slot, is left
/// unused here rather than renumbered.
///
/// VMEX: turning a VMO executable (vmo_replace_as_executable). The kernel
/// mints this one slot and hands it to the first process only.
inline constexpr u64 RSRC_SYSTEM_VMEX_BASE = 1;

/// One past the last System slot this kernel knows.
inline constexpr u64 RSRC_SYSTEM_COUNT = 2;

} // namespace ours

#endif // #ifndef OURS_SYSCALL_RESOURCE_HPP