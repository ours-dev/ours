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
/// What a resource is a capability over. The values are ABI, written out and
/// in a u32, because a kind is a number a program and the kernel will
/// exchange (a resource's info record will report it, and creating a
/// resource will take one in its options), so both have to mean the same
/// thing by it. 3 and 4 are kept out of use: 3 belonged to a root kind the
/// ABI has retired, and 4 is the kind over an ARM CPU's secure monitor
/// calls. Neither is named here, so neither number means anything yet.
///
/// They were implicit, with a Root kind at 3 and System at 4, where a program
/// asking for the secure-monitor kind would have got System. Nothing passes
/// a kind as a number yet (resources are minted only by the kernel), which
/// is what made this the cheap time to renumber.
enum class RsrcKind : u32 {
    MmIo = 0,
    Irq = 1,
    IoPort = 2,
    System = 5,
};

/// A resource's flags. None is named yet; 0x00010000 is kept for an
/// exclusive flag.
enum class RsrcFlag : u32 {

};

/// The slots of the System resource kind: a System resource of size 1 at one
/// of these bases is the capability the slot names, and nothing else is. The
/// values are ABI and never change. Base 0 is a hypervisor's slot, and this
/// kernel has no hypervisor; it is left unused rather than the others
/// renumbered.
///
/// VMEX: turning a VMO executable (vmo_replace_as_executable). The kernel
/// mints this one slot and hands it to the first process only.
inline constexpr u64 RSRC_SYSTEM_VMEX_BASE = 1;

/// Reading bytes from the serial debug console.
inline constexpr u64 RSRC_SYSTEM_DEBUG_BASE = 2;

/// One past the last System slot this kernel knows.
inline constexpr u64 RSRC_SYSTEM_COUNT = 3;

} // namespace ours

#endif // #ifndef OURS_SYSCALL_RESOURCE_HPP