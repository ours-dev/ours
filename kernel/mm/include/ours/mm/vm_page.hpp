/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_PAGE_HPP
#define OURS_MM_VM_PAGE_HPP 1

#include <ours/mm/pm_frame.hpp>
#include <ours/mm/types.hpp>

/// `VmPage` used to be a class deriving from PageFrame, overlaying the same
/// descriptor. It is now an alias of PageFrame (declared in pm_frame.hpp),
/// with the VMM-specific state living in page::VmmBody and reached through
/// PageFrame's role-checked accessors. This header remains so that the many
/// `#include <ours/mm/vm_page.hpp>` sites keep working and keep documenting
/// that they deal with VMM pages.

#endif // #ifndef OURS_MM_VM_PAGE_HPP
