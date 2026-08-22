/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_EXPORTS_HPP
#define OURS_PHYS_EXPORTS_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

/// Marks a declaration as part of physload's module ABI.
///
/// physload carries the whole phys runtime -- consoles, boot memory, the
/// address space, the ELF loader.  A physload_module() is an ET_DYN image
/// with no copy of any of it: the symbols it uses stay undefined at link
/// time and ModuleLoader binds them to physload's definitions when it loads
/// the module.
///
/// The binding only works when both sides agree the symbol is public, and
/// the tree force-includes ours/hidden.hpp, which makes *everything* hidden
/// by default.  A hidden undefined symbol is a link error in the module, so
/// this attribute on the shared declaration is what makes a symbol reachable
/// at all -- which in turn makes the export surface exactly the set of
/// declarations that carry it.
///
/// physload's own link then needs no list: gen-phys-exports.py takes every
/// GLOBAL symbol left with default visibility.
/// PHYS_EXPORT itself lives in ours/compiler.hpp, beside the other attribute
/// macros, so headers outside kernel/phys can carry it without depending on
/// the phys tree.

namespace ours::phys {
/// One row of physload's export table.
///
/// The table is generated from the first link pass and linked into the
/// second, so `name` is the mangled linker name -- the same spelling the
/// module's .dynsym carries.  Layout is fixed: the generator emits it as
/// two pointer-sized words per row.
struct PhysExport {
    char const *name;
    void const *address;
};

/// Look up an exported symbol by its linker name.  Returns nullptr when
/// physload does not export it, which ModuleLoader reports as an unresolved
/// module dependency rather than binding it to zero.
[[nodiscard]]
auto resolve_export(std::string_view name) -> void const *;

/// The export table itself, for diagnostics.
[[nodiscard]]
auto exports() -> std::span<PhysExport const>;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_EXPORTS_HPP
