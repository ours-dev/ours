/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef COMPONENT_FORMAT_HPP
#define COMPONENT_FORMAT_HPP 1

#include <type_traits>

#include <cstddef>

#include <ours/types.hpp>
#include <ustl/util/enum_bits.hpp>

/// The compiled component manifest, `meta/<name>.cm` in the BootFS: a flat
/// layout in the BootFS's style (D2 in
/// docs/design/261004/component-framework.md), a header, fixed-size record
/// tables, then one string table. At tens of records a linear scan of the
/// tables is all a lookup needs, and the header's version leaves room to
/// change the layout.
///
///   Header | Program[program_count] | Arg[arg_count] | Child[child_count]
///          | Capability[capability_count] | Use[use_count]
///          | Offer[offer_count] | Expose[expose_count] | strings
///
/// Each table follows the one before with no gap, so the header's counts
/// alone say where every table starts, and the strings end the file. All
/// values are little-endian, every record size is a multiple of 8, and every
/// u64 sits at a multiple of 8 within its record.
///
/// //system/component_manifest.py writes it, the other half of this
/// contract: each constant and layout here has its twin there.
namespace component {
/// What a capability is. A record of any other value is refused.
enum class Kind : u32 {
    Protocol = 1,
    Directory = 2,
};

/// Where a hop points: the parent, the component itself, or one of its
/// children.
enum class RefKind : u32 {
    Parent = 1,
    Self = 2,
    Child = 3,
};

/// The program's runner. ELF is the only one (non-goals: no other runner).
enum class Runner : u32 {
    Elf = 1,
};

/// When a child starts. Only Eager exists; lazy start, 0 if it comes, is
/// cut (D4) and can be added later without changing the format.
enum class StartupMode : u32 {
    Eager = 1,
};

/// A record's flags word.
enum class RecordFlags : u32 {
    None = 0,

    /// The record states rights. Without it the rights word is zero and the
    /// hop states none, which a route reads differently from an empty set:
    /// a hop that states nothing narrows nothing.
    RightsStated = 1U << 0,

    All = RightsStated,
};
USTL_ENABLE_ENUM_BITMASK(RecordFlags);

/// The rights a directory hop states, by their ours.io wire values
/// (ours_io::Operations).
enum class Rights : u64 {
    None = 0,
    Connect = 0x01,
    ReadBytes = 0x02,
    WriteBytes = 0x04,
    Execute = 0x08,
    GetAttributes = 0x10,
    UpdateAttributes = 0x20,
    Enumerate = 0x40,
    Traverse = 0x80,
    ModifyDirectory = 0x100,

    /// Every bit above; a record with any other is refused.
    All = 0x1FF,

    /// The aliases a manifest spells as r*, rx* and so on.
    RStarDir = Connect | Enumerate | Traverse | ReadBytes | GetAttributes,
    RxStarDir = RStarDir | Execute,
};
USTL_ENABLE_ENUM_BITMASK(Rights);

/// The longest name a manifest may hold: a child's name, a capability's, an
/// `as`. A protocol name this long still fits the one path segment it
/// becomes under /svc (ours_io::MAX_NAME_LENGTH, 255). The compiler enforces
/// it; the decoder does not, since a longer name is a semantic error, not an
/// unsafe one.
inline constexpr u32 MAX_NAME_LENGTH = 100;

/// The longest path or subdir: the longest path an ours.io Open carries
/// (ours_io::MAX_PATH_LENGTH).
inline constexpr u32 MAX_PATH_LENGTH = 1024;
} // namespace component

namespace component::format {
/// "OSCM" as the file's first four bytes.
inline constexpr u32 MAGIC = 0x4D43'534F;
inline constexpr u16 VERSION = 1;

struct Header final {
    u32 magic;
    u16 version;
    u16 reserved; // zero
    u32 total_size;
    u32 string_table_size;
    u32 program_count; // 0 or 1
    u32 arg_count;     // 0 without a program
    u32 child_count;
    u32 capability_count;
    u32 use_count;
    u32 offer_count;
    u32 expose_count;
    u32 reserved2; // zero
};

/// A string: |size| bytes at |offset| in the string table, no NUL. An empty
/// one means the field is not set (a use with no subdir).
struct String final {
    u32 offset;
    u32 size;
};

/// A hop's other end. |child| indexes the Child table when |kind| is Child,
/// and is zero otherwise.
struct Ref final {
    RefKind kind;
    u32 child;
};

/// The runner and the binary, the BootFS path of the program under bin/.
/// Its arguments are the Arg table.
struct Program final {
    Runner runner;
    u32 reserved; // zero
    String binary;
};

struct Arg final {
    String value;
};

/// A child: its name in its parent, and its URL, fuchsia-boot:///#meta/<n>.cm
/// (D26).
struct Child final {
    String name;
    String url;
    StartupMode startup;
    u32 reserved; // zero
};

/// A capability this component provides from its outgoing directory, at
/// svc/<name>.
struct Capability final {
    Kind kind;
    u32 reserved; // zero
    String name;
};

/// What the component asks for, always from its parent (D3): a protocol at
/// /svc/<source_name>, or a directory at |target_path|.
struct Use final {
    Kind kind;
    RecordFlags flags;
    Ref source;
    String source_name;
    String target_path;
    String subdir;
    Rights rights;
};

/// A capability handed from |source| to a child as |target_name|.
struct Offer final {
    Kind kind;
    RecordFlags flags;
    Ref source;
    String source_name;
    Ref target;
    String target_name;
    String subdir;
    Rights rights;
};

/// A capability handed up to the parent as |target_name|.
struct Expose final {
    Kind kind;
    u32 reserved; // zero
    Ref source;
    String source_name;
    Ref target;
    String target_name;
};

template <typename T>
inline constexpr bool IS_RECORD = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

static_assert(IS_RECORD<Header> && sizeof(Header) == 48);
static_assert(offsetof(Header, total_size) == 8);
static_assert(offsetof(Header, program_count) == 16);
static_assert(offsetof(Header, expose_count) == 40);
static_assert(IS_RECORD<String> && sizeof(String) == 8);
static_assert(IS_RECORD<Ref> && sizeof(Ref) == 8);
static_assert(IS_RECORD<Program> && sizeof(Program) == 16);
static_assert(IS_RECORD<Arg> && sizeof(Arg) == 8);
static_assert(IS_RECORD<Child> && sizeof(Child) == 24);
static_assert(offsetof(Child, startup) == 16);
static_assert(IS_RECORD<Capability> && sizeof(Capability) == 16);
static_assert(IS_RECORD<Use> && sizeof(Use) == 48);
static_assert(offsetof(Use, source_name) == 16);
static_assert(offsetof(Use, rights) == 40);
static_assert(IS_RECORD<Offer> && sizeof(Offer) == 56);
static_assert(offsetof(Offer, target) == 24);
static_assert(offsetof(Offer, rights) == 48);
static_assert(IS_RECORD<Expose> && sizeof(Expose) == 40);
static_assert(offsetof(Expose, target) == 24);
} // namespace component::format

#endif // #ifndef COMPONENT_FORMAT_HPP
