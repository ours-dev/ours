/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The decoder against manifests that component_manifest() built from the
/// targets in fixtures/: realm.cm in the default toolchain, its children in
/// the user toolchain. Decoding what the Python compiler wrote is what keeps
/// the two hand-written halves of the format from drifting apart unnoticed.
#include <component/decl.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <cstddef>
#include <cstring>

#include <fixtures/manifests.hpp>
#include <gtest/gtest.h>

namespace {
using component::Decl;
using component::Kind;
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

constexpr auto ECHO = "fuchsia.example.Echo"sv;
constexpr auto LAUNCHER = "fuchsia.process.Launcher"sv;
constexpr auto HOLLOW = "fuchsia.example.Hollow"sv;

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
    EXPECT_EQ(exposes[0].target_name, "fuchsia.example.RealmEcho");
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

    ASSERT_EQ(decl.uses().size(), 1U);
    auto const use = decl.uses()[0];
    EXPECT_EQ(use.kind, Kind::Protocol);
    EXPECT_EQ(use.source.kind, RefKind::Parent);
    EXPECT_EQ(use.source_name, LAUNCHER);
    EXPECT_EQ(use.target_path, "/svc/fuchsia.process.Launcher");
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

    // A protocol list is one record per protocol, each at /svc/<name>.
    constexpr std::array<std::string_view, 3> PROTOCOLS{"echo", LAUNCHER, HOLLOW};
    for (u32 index = 0; index < PROTOCOLS.size(); ++index) {
        auto const use = uses[index + 2];
        EXPECT_EQ(use.kind, Kind::Protocol);
        EXPECT_EQ(use.source_name, PROTOCOLS[index]);
        EXPECT_EQ(use.target_path.substr(0, 5), "/svc/");
        EXPECT_EQ(use.target_path.substr(5), PROTOCOLS[index]);
    }
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
    EXPECT_EQ(refused(offsetof(format::Header, version), u16{2}), Status::NotSupported);
    EXPECT_EQ(refused(offsetof(format::Header, reserved), u16{1}), Status::InvArgs);
    EXPECT_EQ(refused(offsetof(format::Header, reserved2), u32{1}), Status::InvArgs);
    EXPECT_EQ(refused(offsetof(format::Header, program_count), u32{2}), Status::InvArgs);
    // Arguments with no program to take them.
    EXPECT_EQ(refused(offsetof(format::Header, arg_count), u32{1}), Status::InvArgs);
    // A table that would run past the end.
    EXPECT_EQ(refused(offsetof(format::Header, child_count), u32{0xFFFF'FFFF}), Status::InvArgs);
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

TEST(Decl, RefusesANonzeroReservedField) {
    auto const original = bytes_of(fixtures::PROVIDER);
    auto const tables = tables_of(original);
    for (auto const offset: {
             sizeof(format::Header) + offsetof(format::Program, reserved),
             tables.capabilities + offsetof(format::Capability, reserved),
             tables.exposes + offsetof(format::Expose, reserved),
         }) {
        auto bytes = original;
        patch(bytes, offset, u32{1});
        EXPECT_EQ(refusal(bytes), Status::InvArgs) << "offset " << offset;
    }
    auto bytes = bytes_of(fixtures::REALM);
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
                EXPECT_TRUE(inside(bytes, capability.name));
            }
            for (auto const use: decl.uses()) {
                EXPECT_TRUE(inside(bytes, use.source_name) && inside(bytes, use.target_path) &&
                            inside(bytes, use.subdir));
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
                EXPECT_TRUE(inside(bytes, expose.source_name) && inside(bytes, expose.target_name));
            }
        }
    }
}

TEST(Decl, EveryOneByteChangeStaysInBounds) {
    check_every_byte(fixtures::REALM);
    check_every_byte(fixtures::PROVIDER);
    check_every_byte(fixtures::CONSUMER);
    check_every_byte(fixtures::HOLLOW);
}
} // namespace
