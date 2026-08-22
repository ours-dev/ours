/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BUILD_ID_HPP
#define OURS_PHYS_BUILD_ID_HPP 1

#include <string_view>

namespace ours::phys {

/// physload's own GNU build ID, as lowercase hex with no separators -- the
/// same format kernel_elf_interp.gni bakes into a physload_module()'s
/// PT_INTERP (kernel/phys/generate-elf-interp.py's `.lower()`).
///
/// ModuleLoader compares this against a loaded module's PT_INTERP before
/// calling into it: the module and physload share raw struct layouts across
/// their boundary with no ABI versioning of their own, so the one thing that
/// makes that safe is a guarantee they came from the very same link -- which
/// this build-id comparison is what actually enforces, rather than merely
/// documents.
///
/// Reads `.note.gnu.build-id` out of this image's own memory (phys.ld places
/// it in a segment that survives packaging).  Returns an empty view if the
/// note is missing or malformed -- the caller decides whether that is fatal;
/// this function does not panic.
[[nodiscard]]
auto own_build_id() -> std::string_view;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_BUILD_ID_HPP
