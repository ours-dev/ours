/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_BINDER_HPP
#define ELFLOAD_BINDER_HPP 1

#include <elfload/image.hpp>

#include <string_view>

#include <eftl/elf/symbol.hpp>
#include <eftl/elf/traits.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace elfload {
/// Resolves one loaded image's undefined symbols against another's exports,
/// and applies its relocations.
///
/// The exporter is fixed when the Binder is made, because that is what it is:
/// a binder *against* the vDSO. Every program loaded into a process binds
/// against the same one, and the question the constructor answers once -- is
/// this image a dynamic object with a symbol table at all -- should not be
/// re-asked per importer.
///
/// This is deliberately the smallest thing that deserves the name "dynamic
/// linking": one exporter, no DT_NEEDED search, no lookup scope, no lazy
/// binding, no symbol versioning. What is missing is missing because nothing
/// here has two libraries to choose between yet.
///
/// It touches no address space and no kernel object. It is handed an Image --
/// already mapped, with a pointer the caller can write through -- and writes
/// into it. That is the whole reason this is a component of its own rather
/// than part of the loader: everything a relocation pass can get wrong is
/// reachable from a host test with a buffer and two ELF files.
class Binder final {
  public:
    using Bytes = ustl::views::Span<u8 const>;
    using Result = ustl::Result<Binder, ours::Status>;

    /// Reads |file|'s dynamic symbol table -- what it exports -- and remembers
    /// where |image| put it.
    ///
    /// |file| is the ELF as a file, not as it was mapped: the dynamic tables
    /// are read from there, while writes go through the image. Both must
    /// outlive the Binder, which holds views into the file's bytes.
    [[nodiscard]]
    static auto from(Bytes file, Image const &image) -> Result;

    /// Applies |importer|'s relocations, resolving each undefined symbol
    /// against the exporter.
    ///
    /// An importer with no PT_DYNAMIC, or with no relocations, is not an error
    /// -- it imports nothing, which is a legal way for a program to be. A
    /// symbol the exporter does not define is: binding it to zero would turn a
    /// missing entry point into a jump to address zero somewhere later, with
    /// nothing left to say which name was missing.
    [[nodiscard]]
    auto bind(Bytes file, Image const &importer) const -> ours::Status;

  private:
    using Traits = eftl::elf::TraitsNative;
    using Symbols = eftl::elf::SymbolTableView<Traits>;

    Binder(Symbols exports, u64 const bias) : m_exports(exports), m_bias(bias) {}

    /// Finds one name among the exports, returning its link-time value.
    ///
    /// A linear scan, and it stays one while the table is a handful of entries
    /// long: an image binds each import once, at load, and building either
    /// hash table eftl can read (DT_HASH, DT_GNU_HASH) would cost more than
    /// the scans it saves. The tables are in the image; this just does not
    /// need them yet.
    [[nodiscard]]
    auto resolve(std::string_view name) const -> ustl::Option<u64>;

    Symbols m_exports;

    /// The exporter's load bias, added to every definition resolved out of it.
    u64 m_bias;
};
} // namespace elfload

#endif // #ifndef ELFLOAD_BINDER_HPP
