/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The decoder against manifests that component_manifest() built from the
/// targets in fixtures/: realm.cm in the default toolchain, its children in
/// the user toolchain. Decoding what the Python compiler wrote is what keeps
/// the two hand-written halves of the format from drifting apart unnoticed.
#include <component/decl.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstring>

#include <fixtures/manifests.hpp>
#include <gtest/gtest.h>

namespace {
using component::Decl;
using component::Kind;
using component::RecordFlags;
using component::RefKind;
using component::Rights;
using component::StartupMode;
using ours::Status;
namespace format = component::format;

using namespace std::string_view_literals;

template <usize N>
auto bytes_of(std::array<u8, N> const &manifest) -> std::vector<u8> {
    return {manifest.begin(), manifest.end()};
}

auto decode(std::span<u8 const> const bytes) -> Decl {
    auto result = Decl::decode(bytes);
    EXPECT_TRUE(result.is_ok());
    return result.unwrap();
}

auto refusal(std::span<u8 const> const bytes) -> Status {
    auto result = Decl::decode(bytes);
    if (result.is_ok()) {
        return Status::Ok;
    }
    return result.unwrap_err();
}

auto rights(ustl::Option<Rights> const &value) -> u64 {
    EXPECT_TRUE(value.is_some());
    if (value.is_none()) {
        return 0;
    }
    return static_cast<u64>(value.value());
}

/// Where each table of |bytes| starts, worked out again here rather than
/// taken from the decoder, so a mistake in its layout cannot cancel out.
struct Tables final {
    format::Header header;
    usize children;
    usize capabilities;
    usize uses;
    usize offers;
    usize exposes;
};

auto tables_of(std::span<u8 const> const bytes) -> Tables {
    auto tables = Tables{};
    std::memcpy(&tables.header, bytes.data(), sizeof(format::Header));
    auto const &header = tables.header;
    tables.children = sizeof(format::Header) + header.program_count * sizeof(format::Program) +
                      header.arg_count * sizeof(format::Arg);
    tables.capabilities = tables.children + header.child_count * sizeof(format::Child);
    tables.uses = tables.capabilities + header.capability_count * sizeof(format::Capability);
    tables.offers = tables.uses + header.use_count * sizeof(format::Use);
    tables.exposes = tables.offers + header.offer_count * sizeof(format::Offer);
    return tables;
}

template <typename T>
auto patch(std::vector<u8> &bytes, usize const offset, T const value) -> void {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

/// The record of type |Raw| at |offset| of |bytes|, as the compiler wrote it,
/// with no decoder in between.
template <typename Raw>
auto raw_at(std::span<u8 const> const bytes, usize const offset) -> Raw {
    Raw raw{};
    std::memcpy(&raw, bytes.data() + offset, sizeof(Raw));
    return raw;
}

/// Adds |text| to the string table, which ends the file, and answers where.
auto append_string(std::vector<u8> &bytes, std::string_view const text) -> format::String {
    auto header = tables_of(bytes).header;
    auto const string =
        format::String{.offset = header.string_table_size, .size = static_cast<u32>(text.size())};
    bytes.insert(bytes.end(), text.begin(), text.end());
    header.string_table_size += string.size;
    header.total_size += string.size;
    patch(bytes, 0, header);
    return string;
}

constexpr auto ECHO = "ours.example.Echo"sv;
constexpr auto LAUNCHER = "ours.process.Launcher"sv;
constexpr auto HOLLOW = "ours.example.Hollow"sv;

/// PROVIDER with its capability made a directory at /export/data stating r*,
/// and its expose a directory's stating rx* and the subdir shared/docs: the
/// records no fixture compiled today holds, written the way the compiler will
/// write them. The decoder checks structure only, so the rest of the manifest
/// stays a protocol server's.
auto directory_provider() -> std::vector<u8> {
    auto bytes = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(bytes);

    auto const capability = tables.capabilities;
    patch(bytes, capability + offsetof(format::Capability, kind), Kind::Directory);
    patch(bytes, capability + offsetof(format::Capability, flags), RecordFlags::RightsStated);
    patch(bytes, capability + offsetof(format::Capability, rights), Rights::RStarDir);
    patch(bytes, capability + offsetof(format::Capability, path),
          append_string(bytes, "/export/data"));

    auto const expose = tables.exposes;
    patch(bytes, expose + offsetof(format::Expose, kind), Kind::Directory);
    patch(bytes, expose + offsetof(format::Expose, flags), RecordFlags::RightsStated);
    patch(bytes, expose + offsetof(format::Expose, rights), Rights::RxStarDir);
    patch(bytes, expose + offsetof(format::Expose, subdir), append_string(bytes, "shared/docs"));
    return bytes;
}

TEST(Decl, Realm) {
    auto const decl = decode(fixtures::REALM);
    EXPECT_TRUE(decl.program().is_none());
    EXPECT_TRUE(decl.args().is_empty());
    EXPECT_TRUE(decl.capabilities().is_empty());
    EXPECT_TRUE(decl.uses().is_empty());

    auto const children = decl.children();
    ASSERT_EQ(children.size(), 3U);
    constexpr std::array<std::string_view, 3> NAMES{"provider", "consumer", "hollow"};
    constexpr std::array<std::string_view, 3> URLS{
        "fuchsia-boot:///#meta/provider.cm",
        "fuchsia-boot:///#meta/consumer.cm",
        "fuchsia-boot:///#meta/hollow.cm",
    };
    for (u32 index = 0; index < children.size(); ++index) {
        EXPECT_EQ(children[index].name, NAMES[index]);
        EXPECT_EQ(children[index].url, URLS[index]);
        EXPECT_EQ(children[index].startup, StartupMode::Eager);
    }

    auto const offers = decl.offers();
    ASSERT_EQ(offers.size(), 6U);

    auto const boot = offers[0];
    EXPECT_EQ(boot.kind, Kind::Directory);
    EXPECT_EQ(boot.source.kind, RefKind::Parent);
    EXPECT_EQ(boot.source_name, "boot");
    EXPECT_EQ(boot.target.kind, RefKind::Child);
    EXPECT_EQ(children[boot.target.child].name, "consumer");
    EXPECT_EQ(boot.target_name, "boot");
    EXPECT_EQ(boot.subdir, "");
    EXPECT_EQ(rights(boot.rights), static_cast<u64>(Rights::RxStarDir));

    auto const config = offers[1];
    EXPECT_EQ(config.source_name, "boot");
    EXPECT_EQ(config.target_name, "config");
    EXPECT_EQ(config.subdir, "config");
    EXPECT_EQ(rights(config.rights), static_cast<u64>(Rights::RStarDir));

    auto const echo = offers[2];
    EXPECT_EQ(echo.kind, Kind::Protocol);
    EXPECT_EQ(echo.source.kind, RefKind::Child);
    EXPECT_EQ(children[echo.source.child].name, "provider");
    EXPECT_EQ(echo.source_name, ECHO);
    EXPECT_EQ(children[echo.target.child].name, "consumer");
    EXPECT_EQ(echo.target_name, "echo");
    EXPECT_TRUE(echo.rights.is_none());

    // One offer to two targets is one record per target.
    EXPECT_EQ(offers[3].source.kind, RefKind::Parent);
    EXPECT_EQ(offers[3].source_name, LAUNCHER);
    EXPECT_EQ(children[offers[3].target.child].name, "provider");
    EXPECT_EQ(offers[4].source_name, LAUNCHER);
    EXPECT_EQ(children[offers[4].target.child].name, "consumer");

    EXPECT_EQ(children[offers[5].source.child].name, "hollow");
    EXPECT_EQ(offers[5].source_name, HOLLOW);

    auto const exposes = decl.exposes();
    ASSERT_EQ(exposes.size(), 1U);
    EXPECT_EQ(exposes[0].kind, Kind::Protocol);
    EXPECT_EQ(exposes[0].source.kind, RefKind::Child);
    EXPECT_EQ(children[exposes[0].source.child].name, "provider");
    EXPECT_EQ(exposes[0].source_name, ECHO);
    EXPECT_EQ(exposes[0].target.kind, RefKind::Parent);
    EXPECT_EQ(exposes[0].target_name, "ours.example.RealmEcho");
}

TEST(Decl, Provider) {
    auto const decl = decode(fixtures::PROVIDER);
    auto const program = decl.program();
    ASSERT_TRUE(program.is_some());
    EXPECT_EQ(program.value().runner, component::Runner::Elf);
    EXPECT_EQ(program.value().binary, "bin/fixture_program");

    auto names = std::vector<std::string_view>{};
    for (auto const arg: decl.args()) {
        names.push_back(arg);
    }
    EXPECT_EQ(names, (std::vector{"--serve"sv, "echo"sv}));

    ASSERT_EQ(decl.capabilities().size(), 1U);
    EXPECT_EQ(decl.capabilities()[0].kind, Kind::Protocol);
    EXPECT_EQ(decl.capabilities()[0].name, ECHO);
    EXPECT_EQ(decl.capabilities()[0].type, ECHO);

    ASSERT_EQ(decl.uses().size(), 1U);
    auto const use = decl.uses()[0];
    EXPECT_EQ(use.kind, Kind::Protocol);
    EXPECT_EQ(use.source.kind, RefKind::Parent);
    EXPECT_EQ(use.source_name, LAUNCHER);
    EXPECT_EQ(use.target_path, "/svc/ours.process.Launcher");
    EXPECT_EQ(use.type, LAUNCHER);
    EXPECT_TRUE(use.rights.is_none());

    ASSERT_EQ(decl.exposes().size(), 1U);
    EXPECT_EQ(decl.exposes()[0].source.kind, RefKind::Self);
    EXPECT_EQ(decl.exposes()[0].target_name, ECHO);
}

TEST(Decl, Consumer) {
    auto const decl = decode(fixtures::CONSUMER);
    ASSERT_TRUE(decl.program().is_some());
    EXPECT_TRUE(decl.args().is_empty());
    EXPECT_TRUE(decl.children().is_empty());

    auto const uses = decl.uses();
    ASSERT_EQ(uses.size(), 5U);
    EXPECT_EQ(uses[0].kind, Kind::Directory);
    EXPECT_EQ(uses[0].source_name, "boot");
    EXPECT_EQ(uses[0].target_path, "/boot");
    EXPECT_EQ(uses[0].subdir, "");
    EXPECT_EQ(rights(uses[0].rights), static_cast<u64>(Rights::RxStarDir));

    EXPECT_EQ(uses[1].source_name, "config");
    EXPECT_EQ(uses[1].target_path, "/config");
    EXPECT_EQ(uses[1].subdir, "ours");
    EXPECT_EQ(rights(uses[1].rights), static_cast<u64>(Rights::RStarDir));

    // A clause of one protocol and a list of two are one record per
    // protocol, each at /svc/<name>.
    constexpr std::array<std::string_view, 3> PROTOCOLS{"echo", LAUNCHER, HOLLOW};
    for (u32 index = 0; index < PROTOCOLS.size(); ++index) {
        auto const use = uses[index + 2];
        EXPECT_EQ(use.kind, Kind::Protocol);
        EXPECT_EQ(use.source_name, PROTOCOLS[index]);
        EXPECT_EQ(use.target_path.substr(0, 5), "/svc/");
        EXPECT_EQ(use.target_path.substr(5), PROTOCOLS[index]);
    }
}

/// The type a record states is the type it has, and one that states none has
/// its name: a reader never meets an empty type. consumer.cm states the type
/// of `echo`, a name that is not the protocol's; provider.cm states its
/// capability's, which is the name. Every other protocol states none, and so
/// does a directory.
TEST(Decl, ATypeIsWhatTheManifestStatedOrTheName) {
    auto const bytes = bytes_of(fixtures::CONSUMER);
    auto const decl = decode(bytes);
    auto const uses = decl.uses();
    ASSERT_EQ(uses.size(), 5U);
    EXPECT_EQ(uses[0].type, "boot");
    EXPECT_EQ(uses[1].type, "config");
    EXPECT_EQ(uses[2].source_name, "echo");
    EXPECT_EQ(uses[2].type, ECHO);
    EXPECT_EQ(uses[3].type, LAUNCHER);
    EXPECT_EQ(uses[4].type, HOLLOW);

    // What the compiler wrote under that: a string for `echo`, none for the
    // rest. The view above cannot tell a stated type from a filled-in name.
    auto const tables = tables_of(bytes);
    auto const stated = [&bytes, &tables](usize const index) {
        auto const record = tables.uses + index * sizeof(format::Use);
        return raw_at<format::Use>(bytes, record).type.size != 0;
    };
    EXPECT_FALSE(stated(0));
    EXPECT_FALSE(stated(1));
    EXPECT_TRUE(stated(2));
    EXPECT_FALSE(stated(3));
    EXPECT_FALSE(stated(4));

    auto const provider = bytes_of(fixtures::PROVIDER);
    auto const raw_capability =
        raw_at<format::Capability>(provider, tables_of(provider).capabilities);
    EXPECT_NE(raw_capability.type.size, 0U);

    auto const hollow = bytes_of(fixtures::HOLLOW);
    auto const raw_hollow = raw_at<format::Capability>(hollow, tables_of(hollow).capabilities);
    EXPECT_EQ(raw_hollow.type.size, 0U);
    EXPECT_EQ(decode(hollow).capabilities()[0].type, HOLLOW);
}

/// A type that differs from the name comes back as stored, on a capability
/// and on a use alike. The fixtures state none that does on a capability, so
/// the string is pointed at one already in the table: the program's binary.
TEST(Decl, AStoredTypeIsReturnedAsStored) {
    auto const original = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(original);
    auto const binary = raw_at<format::Program>(original, sizeof(format::Header)).binary;

    auto bytes = original;
    patch(bytes, tables.capabilities + offsetof(format::Capability, type), binary);
    EXPECT_EQ(decode(bytes).capabilities()[0].type, "bin/fixture_program");
    EXPECT_EQ(decode(bytes).capabilities()[0].name, ECHO);

    bytes = original;
    patch(bytes, tables.uses + offsetof(format::Use, type), binary);
    EXPECT_EQ(decode(bytes).uses()[0].type, "bin/fixture_program");
    EXPECT_EQ(decode(bytes).uses()[0].source_name, LAUNCHER);
}

TEST(Decl, Hollow) {
    auto const decl = decode(fixtures::HOLLOW);
    EXPECT_TRUE(decl.program().is_none());
    ASSERT_EQ(decl.capabilities().size(), 1U);
    EXPECT_EQ(decl.capabilities()[0].name, HOLLOW);
    ASSERT_EQ(decl.exposes().size(), 1U);
    EXPECT_EQ(decl.exposes()[0].source.kind, RefKind::Self);
}

TEST(Decl, RangeForVisitsEveryRecord) {
    auto const decl = decode(fixtures::REALM);
    auto count = u32{0};
    for (auto const offer: decl.offers()) {
        EXPECT_EQ(offer.target.kind, RefKind::Child);
        ++count;
    }
    EXPECT_EQ(count, decl.offers().size());
}

/// A Records is a view, as a std::span is: two calls to decl.offers() view
/// the same table, so their iterators compare as positions in it, and an
/// iterator stays valid after the Records it came from is gone. The router's
/// natural search, std::find_if from decl.offers().begin() to
/// decl.offers().end(), needs both: those are two temporaries.
TEST(Decl, IteratorsAreViewsOfTheTable) {
    auto const decl = decode(fixtures::REALM);
    EXPECT_TRUE(decl.offers().end() == decl.offers().end());
    EXPECT_FALSE(decl.offers().begin() == decl.offers().end());

    auto const missing =
        std::find_if(decl.offers().begin(), decl.offers().end(),
                     [](component::Offer const &offer) { return offer.target_name == "none"; });
    EXPECT_TRUE(missing == decl.offers().end());

    // The Records this came from is destroyed at the end of the statement.
    auto const first = decl.children().begin();
    EXPECT_EQ((*first).name, "provider");
}

TEST(DeclDeathTest, IndexPastTheEndTraps) {
    auto const decl = decode(fixtures::REALM);
    EXPECT_DEATH(
        {
            // Read for the trap alone.
            auto _ = decl.children()[3];
        },
        "");
}

TEST(Decl, RefusesNoBytes) {
    EXPECT_EQ(refusal({}), Status::InvArgs);
}

TEST(Decl, RefusesEveryTruncation) {
    auto const bytes = bytes_of(fixtures::REALM);
    for (usize size = 0; size < bytes.size(); ++size) {
        EXPECT_NE(refusal({bytes.data(), size}), Status::Ok) << "size " << size;
    }
}

TEST(Decl, RefusesTrailingBytes) {
    auto bytes = bytes_of(fixtures::REALM);
    bytes.push_back(0);
    EXPECT_EQ(refusal(bytes), Status::InvArgs);

    // With total_size saying so too, the tables still do not end at the end.
    patch(bytes, offsetof(format::Header, total_size), static_cast<u32>(bytes.size()));
    EXPECT_EQ(refusal(bytes), Status::InvArgs);
}

TEST(Decl, RefusesABadHeader) {
    auto const original = bytes_of(fixtures::REALM);
    auto const refused = [&original](usize const offset, auto const value) {
        auto bytes = original;
        patch(bytes, offset, value);
        return refusal(bytes);
    };
    EXPECT_EQ(refused(offsetof(format::Header, magic), u32{0x4D43534E}), Status::InvArgs);
    EXPECT_EQ(refused(offsetof(format::Header, version), u16{1}), Status::NotSupported);
    EXPECT_EQ(refused(offsetof(format::Header, version), u16{2}), Status::NotSupported);
    EXPECT_EQ(refused(offsetof(format::Header, version), u16{4}), Status::NotSupported);
    EXPECT_EQ(refused(offsetof(format::Header, reserved), u16{1}), Status::InvArgs);
    EXPECT_EQ(refused(offsetof(format::Header, reserved2), u32{1}), Status::InvArgs);
    EXPECT_EQ(refused(offsetof(format::Header, program_count), u32{2}), Status::InvArgs);
    // Arguments with no program to take them.
    EXPECT_EQ(refused(offsetof(format::Header, arg_count), u32{1}), Status::InvArgs);
    // A table that would run past the end.
    EXPECT_EQ(refused(offsetof(format::Header, child_count), u32{0xFFFF'FFFF}), Status::InvArgs);
}

/// |manifest| as an older version laid it out, with |version| in the header
/// and total_size to match. Version 2 had a Capability of 24 bytes, ending at
/// its `type` (|path| and |rights| came after), and an Expose of 40, ending at
/// its target name (|subdir| and |rights| came after): the first bytes of
/// version 3's records, which only grew at their ends. Version 1 had neither
/// `type` string, of a Capability nor of a Use. The string table is left as
/// it is, so every string still resolves.
auto as_older_version(std::span<u8 const> const manifest, u16 const version) -> std::vector<u8> {
    auto const tables = tables_of(manifest);
    auto bytes = std::vector<u8>(manifest.begin(), manifest.begin() + tables.capabilities);
    auto const append = [&bytes, &manifest](usize const from, usize const size) {
        bytes.insert(bytes.end(), manifest.begin() + from, manifest.begin() + from + size);
    };
    auto const capability_size = [version] {
        if (version == 1) {
            return offsetof(format::Capability, type);
        }
        return offsetof(format::Capability, path);
    }();
    for (u32 index = 0; index < tables.header.capability_count; ++index) {
        append(tables.capabilities + static_cast<usize>(index) * sizeof(format::Capability),
               capability_size);
    }
    for (u32 index = 0; index < tables.header.use_count; ++index) {
        auto const record = tables.uses + static_cast<usize>(index) * sizeof(format::Use);
        if (version == 1) {
            append(record, offsetof(format::Use, type));
            append(record + offsetof(format::Use, rights), sizeof(Rights));
        } else {
            append(record, sizeof(format::Use));
        }
    }
    append(tables.offers, tables.header.offer_count * sizeof(format::Offer));
    for (u32 index = 0; index < tables.header.expose_count; ++index) {
        append(tables.exposes + static_cast<usize>(index) * sizeof(format::Expose),
               offsetof(format::Expose, subdir));
    }
    append(tables.exposes + tables.header.expose_count * sizeof(format::Expose),
           tables.header.string_table_size);
    patch(bytes, offsetof(format::Header, version), version);
    patch(bytes, offsetof(format::Header, total_size), static_cast<u32>(bytes.size()));
    return bytes;
}

/// A version-1 or version-2 file is refused for its version, before its
/// records are read as version 3's, which are bigger: they would be taken for
/// others. Each file below is whole in its own version's layout, so the
/// version is what refuses it: patched to 3, the same bytes are refused for
/// their size instead. The manifests are those with a record that changed:
/// a Capability or an Expose for version 2, a `type` string for version 1.
TEST(Decl, RefusesAnOlderFile) {
    EXPECT_EQ(format::VERSION, u16{3});
    auto const check = [](u16 const version, std::vector<u8> const &manifest) {
        auto bytes = as_older_version(manifest, version);
        ASSERT_LT(bytes.size(), manifest.size()) << "version " << version;
        EXPECT_EQ(refusal(bytes), Status::NotSupported) << "version " << version;

        patch(bytes, offsetof(format::Header, version), format::VERSION);
        EXPECT_EQ(refusal(bytes), Status::InvArgs) << "version " << version;
    };
    for (auto const &manifest: {bytes_of(fixtures::PROVIDER), bytes_of(fixtures::CONSUMER)}) {
        check(1, manifest);
    }
    for (auto const &manifest: {bytes_of(fixtures::PROVIDER), bytes_of(fixtures::HOLLOW),
                                bytes_of(fixtures::REALM), bytes_of(fixtures::TOP)}) {
        check(2, manifest);
    }
}

TEST(Decl, RefusesAStringOutsideTheTable) {
    auto const original = bytes_of(fixtures::REALM);
    auto const tables = tables_of(original);
    auto const name = tables.children + offsetof(format::Child, name);
    auto const table_size = tables.header.string_table_size;

    auto bytes = original;
    patch(bytes, name, format::String{.offset = table_size, .size = 1});
    EXPECT_EQ(refusal(bytes), Status::OutOfRange);

    // Ending exactly at the end of the table is inside it.
    bytes = original;
    patch(bytes, name, format::String{.offset = table_size - 1, .size = 1});
    EXPECT_EQ(refusal(bytes), Status::Ok);

    // An offset near 4 GiB must not wrap back inside.
    bytes = original;
    patch(bytes, name, format::String{.offset = 0xFFFF'FFFF, .size = 2});
    EXPECT_EQ(refusal(bytes), Status::OutOfRange);
}

/// The type of a capability and of a use is a string like any other: it lies
/// inside the table, and one that ends exactly at the end is inside.
TEST(Decl, RefusesATypeOutsideTheTable) {
    auto const original = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(original);
    auto const table_size = tables.header.string_table_size;
    for (auto const offset: {
             tables.capabilities + offsetof(format::Capability, type),
             tables.uses + offsetof(format::Use, type),
         }) {
        auto bytes = original;
        patch(bytes, offset, format::String{.offset = table_size, .size = 1});
        EXPECT_EQ(refusal(bytes), Status::OutOfRange) << "offset " << offset;

        bytes = original;
        patch(bytes, offset, format::String{.offset = table_size - 1, .size = 1});
        EXPECT_EQ(refusal(bytes), Status::Ok) << "offset " << offset;

        // An offset near 4 GiB must not wrap back inside.
        bytes = original;
        patch(bytes, offset, format::String{.offset = 0xFFFF'FFFF, .size = 2});
        EXPECT_EQ(refusal(bytes), Status::OutOfRange) << "offset " << offset;

        bytes = original;
        patch(bytes, offset, format::String{.offset = 0, .size = table_size + 1});
        EXPECT_EQ(refusal(bytes), Status::OutOfRange) << "offset " << offset;
    }
}

/// The format is version 3, and the two records that grew are the size and
/// have the fields at the offsets that //system/component_manifest.py writes
/// (its EncodeTest says the same numbers). Everything the header counts is
/// still read from the file the compiler wrote.
TEST(Decl, TheFormatIsVersionThree) {
    EXPECT_EQ(format::VERSION, u16{3});
    for (auto const &manifest: {bytes_of(fixtures::REALM), bytes_of(fixtures::PROVIDER),
                                bytes_of(fixtures::CONSUMER), bytes_of(fixtures::HOLLOW)}) {
        EXPECT_EQ(raw_at<format::Header>(manifest, 0).version, u16{3});
    }

    EXPECT_EQ(sizeof(format::Capability), 40U);
    EXPECT_EQ(offsetof(format::Capability, kind), 0U);
    EXPECT_EQ(offsetof(format::Capability, flags), 4U);
    EXPECT_EQ(offsetof(format::Capability, name), 8U);
    EXPECT_EQ(offsetof(format::Capability, type), 16U);
    EXPECT_EQ(offsetof(format::Capability, path), 24U);
    EXPECT_EQ(offsetof(format::Capability, rights), 32U);

    EXPECT_EQ(sizeof(format::Expose), 56U);
    EXPECT_EQ(offsetof(format::Expose, flags), 4U);
    EXPECT_EQ(offsetof(format::Expose, source), 8U);
    EXPECT_EQ(offsetof(format::Expose, source_name), 16U);
    EXPECT_EQ(offsetof(format::Expose, target), 24U);
    EXPECT_EQ(offsetof(format::Expose, target_name), 32U);
    EXPECT_EQ(offsetof(format::Expose, subdir), 40U);
    EXPECT_EQ(offsetof(format::Expose, rights), 48U);
}

/// Every capability and expose the compiler writes today states no path, no
/// subdir and no rights, so the new fields read as empty and none, and the
/// tables still hold what they held: the header's counts say how many.
TEST(Decl, TheNewFieldsOfTheManifestsAboveAreEmpty) {
    for (auto const &manifest:
         {bytes_of(fixtures::REALM), bytes_of(fixtures::PROVIDER), bytes_of(fixtures::HOLLOW),
          bytes_of(fixtures::TOP), bytes_of(fixtures::USER)}) {
        auto const decl = decode(manifest);
        for (auto const capability: decl.capabilities()) {
            EXPECT_EQ(capability.path, "");
            EXPECT_TRUE(capability.rights.is_none());
        }
        for (auto const expose: decl.exposes()) {
            EXPECT_EQ(expose.subdir, "");
            EXPECT_TRUE(expose.rights.is_none());
        }
    }
    auto const provider = decode(fixtures::PROVIDER);
    EXPECT_EQ(provider.capabilities()[0].name, ECHO);
    EXPECT_EQ(provider.exposes()[0].target_name, ECHO);
}

/// A declaration with a path and rights, and an expose with a subdir and
/// rights, read back through the views. The kind is what the patch set; the
/// rest of each record is the fixture's.
TEST(Decl, ADirectoryStatesItsPathAndRightsAndAnExposeItsSubdirAndRights) {
    auto const bytes = directory_provider();
    auto const decl = decode(bytes);

    ASSERT_EQ(decl.capabilities().size(), 1U);
    auto const capability = decl.capabilities()[0];
    EXPECT_EQ(capability.kind, Kind::Directory);
    EXPECT_EQ(capability.name, ECHO);
    EXPECT_EQ(capability.path, "/export/data");
    EXPECT_EQ(rights(capability.rights), static_cast<u64>(Rights::RStarDir));

    ASSERT_EQ(decl.exposes().size(), 1U);
    auto const expose = decl.exposes()[0];
    EXPECT_EQ(expose.kind, Kind::Directory);
    EXPECT_EQ(expose.source.kind, RefKind::Self);
    EXPECT_EQ(expose.target_name, ECHO);
    EXPECT_EQ(expose.subdir, "shared/docs");
    EXPECT_EQ(rights(expose.rights), static_cast<u64>(Rights::RxStarDir));
}

/// A stated empty set is not an unstated one: a hop that states no rights
/// narrows nothing, and one that states none of them allows nothing.
TEST(Decl, AStatedEmptySetOfRightsIsNotNone) {
    auto bytes = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(bytes);
    patch(bytes, tables.capabilities + offsetof(format::Capability, flags),
          RecordFlags::RightsStated);
    patch(bytes, tables.exposes + offsetof(format::Expose, flags), RecordFlags::RightsStated);
    auto const decl = decode(bytes);
    ASSERT_TRUE(decl.capabilities()[0].rights.is_some());
    EXPECT_EQ(decl.capabilities()[0].rights.value(), Rights::None);
    ASSERT_TRUE(decl.exposes()[0].rights.is_some());
    EXPECT_EQ(decl.exposes()[0].rights.value(), Rights::None);
}

/// The new strings are strings like any other: inside the table, and one that
/// ends exactly at the end is inside.
TEST(Decl, RefusesAPathOrASubdirOutsideTheTable) {
    auto const original = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(original);
    auto const table_size = tables.header.string_table_size;
    for (auto const offset: {
             tables.capabilities + offsetof(format::Capability, path),
             tables.exposes + offsetof(format::Expose, subdir),
         }) {
        auto bytes = original;
        patch(bytes, offset, format::String{.offset = table_size, .size = 1});
        EXPECT_EQ(refusal(bytes), Status::OutOfRange) << "offset " << offset;

        bytes = original;
        patch(bytes, offset, format::String{.offset = table_size - 1, .size = 1});
        EXPECT_EQ(refusal(bytes), Status::Ok) << "offset " << offset;

        // An offset near 4 GiB must not wrap back inside.
        bytes = original;
        patch(bytes, offset, format::String{.offset = 0xFFFF'FFFF, .size = 2});
        EXPECT_EQ(refusal(bytes), Status::OutOfRange) << "offset " << offset;

        bytes = original;
        patch(bytes, offset, format::String{.offset = 0, .size = table_size + 1});
        EXPECT_EQ(refusal(bytes), Status::OutOfRange) << "offset " << offset;
    }
}

/// Rights go through the function a use already goes through, on both records:
/// without the flag the word must be zero, with it only the bits the format
/// defines are allowed, and an unknown flag is refused.
TEST(Decl, RefusesBadRightsAndFlagsOnACapabilityAndAnExpose) {
    auto const original = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(original);
    struct Record final {
        char const *name;
        usize flags;
        usize rights;
    };
    constexpr auto STATED = std::to_underlying(RecordFlags::RightsStated);
    for (auto const &record: {
             Record{"capability", tables.capabilities + offsetof(format::Capability, flags),
                    tables.capabilities + offsetof(format::Capability, rights)},
             Record{"expose", tables.exposes + offsetof(format::Expose, flags),
                    tables.exposes + offsetof(format::Expose, rights)},
         }) {
        auto const refused = [&](u32 const flags, u64 const rights) {
            auto bytes = original;
            patch(bytes, record.flags, flags);
            patch(bytes, record.rights, rights);
            return refusal(bytes);
        };
        EXPECT_EQ(refused(0, 0), Status::Ok) << record.name;
        // Rights with no flag to state them.
        EXPECT_EQ(refused(0, 1), Status::InvArgs) << record.name;
        // Stated: any known bit, all of them, none.
        EXPECT_EQ(refused(STATED, 0x1), Status::Ok) << record.name;
        EXPECT_EQ(refused(STATED, 0x1FF), Status::Ok) << record.name;
        EXPECT_EQ(refused(STATED, 0), Status::Ok) << record.name;
        // Stated, with a bit the format does not define.
        EXPECT_EQ(refused(STATED, 0x200), Status::InvArgs) << record.name;
        EXPECT_EQ(refused(STATED, u64{1} << 63), Status::InvArgs) << record.name;
        // A flag the format does not define, alone and beside the one it does.
        EXPECT_EQ(refused(2, 0), Status::InvArgs) << record.name;
        EXPECT_EQ(refused(STATED | 2, 0), Status::InvArgs) << record.name;
    }
}

TEST(Decl, RefusesAChildIndexThatDoesNotResolve) {
    auto const original = bytes_of(fixtures::REALM);
    auto const tables = tables_of(original);
    auto const target = tables.offers + offsetof(format::Offer, target);

    auto bytes = original;
    patch(bytes, target, format::Ref{.kind = RefKind::Child, .child = 3});
    EXPECT_EQ(refusal(bytes), Status::OutOfRange);

    bytes = original;
    patch(bytes, target, format::Ref{.kind = RefKind::Child, .child = 2});
    EXPECT_EQ(refusal(bytes), Status::Ok);

    // A parent has no index: a stray one is refused, not ignored.
    bytes = original;
    patch(bytes, tables.offers + offsetof(format::Offer, source),
          format::Ref{.kind = RefKind::Parent, .child = 1});
    EXPECT_EQ(refusal(bytes), Status::InvArgs);
}

TEST(Decl, RefusesUnknownValues) {
    auto const original = bytes_of(fixtures::REALM);
    auto const tables = tables_of(original);
    auto const refused = [&original](usize const offset, auto const value) {
        auto bytes = original;
        patch(bytes, offset, value);
        return refusal(bytes);
    };
    auto const offer = tables.offers;
    EXPECT_EQ(refused(offer + offsetof(format::Offer, kind), u32{3}), Status::InvArgs);
    EXPECT_EQ(refused(offer + offsetof(format::Offer, kind), u32{0}), Status::InvArgs);
    EXPECT_EQ(refused(offer + offsetof(format::Offer, source), u32{4}), Status::InvArgs);
    EXPECT_EQ(refused(offer + offsetof(format::Offer, flags), u32{2}), Status::InvArgs);
    EXPECT_EQ(refused(offer + offsetof(format::Offer, rights), u64{0x200}), Status::InvArgs);
    // Offer 2 is a protocol, which states no rights, so it may carry none.
    EXPECT_EQ(refused(offer + 2 * sizeof(format::Offer) + offsetof(format::Offer, rights), u64{1}),
              Status::InvArgs);
    // LAZY is cut (D4), so 0 is no startup mode at all here.
    EXPECT_EQ(refused(tables.children + offsetof(format::Child, startup), u32{0}), Status::InvArgs);
    EXPECT_EQ(refused(tables.exposes + offsetof(format::Expose, target), u32{4}), Status::InvArgs);

    auto const provider = bytes_of(fixtures::PROVIDER);
    auto const provider_tables = tables_of(provider);
    auto bytes = provider;
    patch(bytes, sizeof(format::Header) + offsetof(format::Program, runner), u32{2});
    EXPECT_EQ(refusal(bytes), Status::InvArgs);
    bytes = provider;
    patch(bytes, provider_tables.capabilities + offsetof(format::Capability, kind), u32{7});
    EXPECT_EQ(refusal(bytes), Status::InvArgs);
}

/// The records that still have a reserved word. A Capability's and an Expose's
/// became their flags in version 3.
TEST(Decl, RefusesANonzeroReservedField) {
    auto const original = bytes_of(fixtures::PROVIDER);
    auto bytes = original;
    patch(bytes, sizeof(format::Header) + offsetof(format::Program, reserved), u32{1});
    EXPECT_EQ(refusal(bytes), Status::InvArgs);

    bytes = bytes_of(fixtures::REALM);
    patch(bytes, tables_of(bytes).children + offsetof(format::Child, reserved), u32{1});
    EXPECT_EQ(refusal(bytes), Status::InvArgs);
}

/// Whatever one byte is changed to, the decoder either refuses the manifest
/// or every string it hands out lies inside the bytes: the claim "structure
/// only is enough to stay memory-safe", checked byte by byte.
auto check_every_byte(std::span<u8 const> const manifest) -> void {
    auto const inside = [](std::vector<u8> const &bytes, std::string_view const text) {
        auto const *const begin = reinterpret_cast<char const *>(bytes.data());
        return text.data() >= begin && text.data() + text.size() <= begin + bytes.size();
    };
    for (usize index = 0; index < manifest.size(); ++index) {
        for (auto const value: {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF, manifest[index] ^ 0x01}) {
            auto bytes = std::vector<u8>(manifest.begin(), manifest.end());
            bytes[index] = static_cast<u8>(value);
            auto result = Decl::decode(bytes);
            if (result.is_err()) {
                continue;
            }
            auto const decl = result.unwrap();
            auto const program = decl.program();
            if (program.is_some()) {
                EXPECT_TRUE(inside(bytes, program.value().binary));
            }
            for (auto const arg: decl.args()) {
                EXPECT_TRUE(inside(bytes, arg));
            }
            for (auto const child: decl.children()) {
                EXPECT_TRUE(inside(bytes, child.name) && inside(bytes, child.url));
            }
            for (auto const capability: decl.capabilities()) {
                EXPECT_TRUE(inside(bytes, capability.name) && inside(bytes, capability.type) &&
                            inside(bytes, capability.path));
            }
            for (auto const use: decl.uses()) {
                EXPECT_TRUE(inside(bytes, use.source_name) && inside(bytes, use.target_path) &&
                            inside(bytes, use.subdir) && inside(bytes, use.type));
            }
            for (auto const offer: decl.offers()) {
                EXPECT_TRUE(inside(bytes, offer.source_name) && inside(bytes, offer.target_name) &&
                            inside(bytes, offer.subdir));
                for (auto const ref: {offer.source, offer.target}) {
                    if (ref.kind == RefKind::Child) {
                        EXPECT_LT(ref.child, decl.children().size());
                    }
                }
            }
            for (auto const expose: decl.exposes()) {
                EXPECT_TRUE(inside(bytes, expose.source_name) &&
                            inside(bytes, expose.target_name) && inside(bytes, expose.subdir));
            }
        }
    }
}

TEST(Decl, EveryOneByteChangeStaysInBounds) {
    check_every_byte(fixtures::REALM);
    check_every_byte(fixtures::PROVIDER);
    check_every_byte(fixtures::CONSUMER);
    check_every_byte(fixtures::HOLLOW);
    // The records with a path, a subdir and rights stated, which no fixture
    // above holds.
    check_every_byte(directory_provider());
}
} // namespace
