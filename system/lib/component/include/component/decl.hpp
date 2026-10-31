/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef COMPONENT_DECL_HPP
#define COMPONENT_DECL_HPP 1

#include <component/format.hpp>

#include <span>
#include <string_view>

#include <cstring>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace component {
/// A hop's other end: the parent, the component itself, or a child by its
/// index in Decl::children(), which the decoder checked.
using Ref = format::Ref;

/// A decoded manifest's records, as the router and the root read them: the
/// format's records with every string turned into a view of the manifest's
/// bytes, which must outlive them.

struct Program final {
    Runner runner;
    /// The program's BootFS path, under bin/.
    std::string_view binary;
};

struct Child final {
    std::string_view name;
    std::string_view url;
    StartupMode startup;
};

struct Capability final {
    Kind kind;
    std::string_view name;
    /// The protocol this capability speaks. The name when the manifest states
    /// no type, so a reader never sees an empty one. Only a protocol's type
    /// means anything: a directory has none, and its view holds the name.
    std::string_view type;
    /// Where a directory capability sits in the provider's outgoing
    /// directory, as the manifest wrote it. Empty for a protocol. The decoder
    /// does not check that a directory states one or that it is absolute.
    std::string_view path;
    /// The most a directory capability allows. None when the declaration
    /// states none, as a protocol's never does; a route reads that as a
    /// declaration that narrows nothing.
    ustl::Option<Rights> rights;
};

struct Use final {
    Kind kind;
    Ref source;
    std::string_view source_name;
    /// Where the namespace entry goes: /svc/<name> for a protocol (D3).
    std::string_view target_path;
    /// Empty when the use names none.
    std::string_view subdir;
    /// The protocol the component speaks over this use. The source name when
    /// the manifest states no type, so a reader never sees an empty one. Only
    /// a protocol use's type means anything: a directory use never states one.
    std::string_view type;
    /// None when the use states none, as a protocol use never does.
    ustl::Option<Rights> rights;
};

struct Offer final {
    Kind kind;
    Ref source;
    std::string_view source_name;
    Ref target;
    std::string_view target_name;
    std::string_view subdir;
    ustl::Option<Rights> rights;
};

struct Expose final {
    Kind kind;
    Ref source;
    std::string_view source_name;
    Ref target;
    std::string_view target_name;
    /// Empty when the expose names none, as a protocol expose never does.
    std::string_view subdir;
    /// None when the expose states none, as a protocol expose never does: an
    /// expose that states none narrows nothing.
    ustl::Option<Rights> rights;
};

namespace detail {
    /// The string table of a decoded manifest.
    class Strings final {
      public:
        constexpr Strings() = default;
        constexpr explicit Strings(u8 const *data) : m_data(data) {}

        /// |string|, which the decoder checked lies inside the table.
        [[nodiscard]]
        auto text(format::String const &string) const -> std::string_view {
            return {reinterpret_cast<char const *>(m_data + string.offset), string.size};
        }

      private:
        u8 const *m_data = nullptr;
    };

    [[nodiscard]]
    auto view(format::Arg const &raw, Strings const &strings) -> std::string_view;
    [[nodiscard]]
    auto view(format::Child const &raw, Strings const &strings) -> Child;
    [[nodiscard]]
    auto view(format::Capability const &raw, Strings const &strings) -> Capability;
    [[nodiscard]]
    auto view(format::Use const &raw, Strings const &strings) -> Use;
    [[nodiscard]]
    auto view(format::Offer const &raw, Strings const &strings) -> Offer;
    [[nodiscard]]
    auto view(format::Expose const &raw, Strings const &strings) -> Expose;
} // namespace detail

/// One record table of a decoded manifest. Iterable with a range-for, and
/// indexable: `decl.children()[offer.target.child]`. Each access copies the
/// record out of the bytes, so the manifest needs no particular alignment.
///
/// A view, as std::span is: copying one copies three words, and every
/// Records over a table, like every iterator into it, depends only on the
/// manifest's bytes, never on the Records object it came from.
template <typename Element, typename Raw>
class Records final {
  public:
    using Size = u32;

    /// Holds the view's state itself rather than a pointer to the Records
    /// that made it. With a pointer, `decl.offers().begin()` would dangle
    /// once that temporary died, and iterators from two calls to
    /// decl.offers() would never compare equal, so a search from one call's
    /// begin() to another's end() would run past the table.
    class Iter final {
      public:
        [[nodiscard]]
        auto operator*() const -> Element {
            return Records(m_table, m_count, m_strings)[m_index];
        }

        auto operator++() -> Iter & {
            ++m_index;
            return *this;
        }

        /// Positions in one table compare equal, whichever view they came
        /// from.
        [[nodiscard]]
        auto operator==(Iter const &other) const -> bool {
            return m_table == other.m_table && m_index == other.m_index;
        }

      private:
        friend class Records;
        Iter(Records const &records, Size index)
            : m_table(records.m_table)
            , m_count(records.m_count)
            , m_strings(records.m_strings)
            , m_index(index) {}

        u8 const *m_table;
        Size m_count;
        detail::Strings m_strings;
        Size m_index;
    };

    [[nodiscard]]
    auto size() const -> Size {
        return m_count;
    }

    [[nodiscard]]
    auto is_empty() const -> bool {
        return m_count == 0;
    }

    /// The record at |index|. An index past the end is the caller's bug and
    /// traps, as ustl::Option does on an empty access: a Ref's child index
    /// never is, since the decoder checked every one.
    [[nodiscard]]
    auto operator[](Size const index) const -> Element {
        if (index >= m_count) {
            __builtin_trap();
        }
        Raw raw{};
        std::memcpy(&raw, m_table + static_cast<usize>(index) * sizeof(Raw), sizeof(Raw));
        return detail::view(raw, m_strings);
    }

    [[nodiscard]]
    auto begin() const -> Iter {
        return Iter(*this, 0);
    }

    [[nodiscard]]
    auto end() const -> Iter {
        return Iter(*this, m_count);
    }

  private:
    friend class Decl;
    Records(u8 const *table, Size count, detail::Strings strings)
        : m_table(table), m_count(count), m_strings(strings) {}

    u8 const *m_table;
    Size m_count;
    detail::Strings m_strings;
};

/// A compiled manifest, decoded in place.
///
/// The decoder checks structure only: the header, that every table and the
/// string table fit the bytes exactly, that every string lies inside the
/// string table, that every enum value and flag is one the format defines,
/// that rights are stated only under the flag that states them and name only
/// known bits, that reserved fields are zero, and that every child index
/// resolves. That is enough for any input to be read without going out of
/// bounds. Whether the manifest makes sense -- unique names, `from: "self"`
/// naming a declared capability, no offer cycle, paths that do not nest, a
/// directory declaration that states a path and rights and a protocol one
/// that states neither -- the compiler checked when it wrote the file
/// (//system/component_manifest.py), and the build alone writes the BootFS
/// the root reads it from, so the root does not run those checks again when
/// it resolves a component; the design records that choice.
class Decl final {
  public:
    using Result = ustl::Result<Decl, ours::Status>;

    /// Decodes |data|, which must outlive the Decl and everything read from
    /// it. InvArgs for a malformed manifest: a wrong magic, a size that is
    /// not the header's, an unknown enum value or flag, a reserved field that
    /// is not zero, rights on a record that states none, two programs, or
    /// arguments without one. OutOfRange for a string or a child index that
    /// points outside its table. NotSupported for a version other than
    /// format::VERSION.
    [[nodiscard]]
    static auto decode(std::span<u8 const> data) -> Result;

    /// The program, or none for a component without one.
    [[nodiscard]]
    auto program() const -> ustl::Option<Program>;

    /// The program's arguments, after argv[0].
    [[nodiscard]]
    auto args() const -> Records<std::string_view, format::Arg>;

    [[nodiscard]]
    auto children() const -> Records<Child, format::Child>;

    [[nodiscard]]
    auto capabilities() const -> Records<Capability, format::Capability>;

    [[nodiscard]]
    auto uses() const -> Records<Use, format::Use>;

    [[nodiscard]]
    auto offers() const -> Records<Offer, format::Offer>;

    [[nodiscard]]
    auto exposes() const -> Records<Expose, format::Expose>;

  private:
    /// Where each table starts, from the start of the manifest.
    struct Layout final {
        usize programs;
        usize args;
        usize children;
        usize capabilities;
        usize uses;
        usize offers;
        usize exposes;
        usize strings;
        usize end;
    };

    Decl(u8 const *data, format::Header const &header, Layout const &layout)
        : m_data(data), m_header(header), m_layout(layout), m_strings(data + layout.strings) {}

    template <typename Element, typename Raw>
    [[nodiscard]]
    auto records(usize const offset, u32 const count) const -> Records<Element, Raw> {
        return Records<Element, Raw>(m_data + offset, count, m_strings);
    }

    u8 const *m_data;
    format::Header m_header;
    Layout m_layout;
    detail::Strings m_strings;
};
} // namespace component

#endif // #ifndef COMPONENT_DECL_HPP
