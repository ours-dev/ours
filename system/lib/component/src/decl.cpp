/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <component/decl.hpp>

#include <initializer_list>

namespace component {
namespace {
    using ours::Status;

    /// The record of type |Raw| at |offset|, copied out, so that the bytes need
    /// no alignment and no object of that type has to live in them.
    template <typename Raw>
    [[nodiscard]]
    auto read(u8 const *data, usize const offset) -> Raw {
        Raw raw{};
        std::memcpy(&raw, data + offset, sizeof(Raw));
        return raw;
    }

    /// The first status of |statuses| that is not Ok, or Ok.
    [[nodiscard]]
    auto first_error(std::initializer_list<Status> const statuses) -> Status {
        for (auto const status: statuses) {
            if (status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    /// What a record's references are checked against: the size of the string
    /// table and the number of children.
    struct Bounds final {
        u32 strings;
        u32 children;
    };

    [[nodiscard]]
    auto check_kind(Kind const kind) -> Status {
        if (kind != Kind::Protocol && kind != Kind::Directory) {
            return Status::InvArgs;
        }
        return Status::Ok;
    }

    /// Whether |string| lies inside the string table. The sum is taken in 64
    /// bits, so an offset near 4 GiB cannot wrap back inside.
    [[nodiscard]]
    auto check_string(format::String const &string, Bounds const &bounds) -> Status {
        if (static_cast<u64>(string.offset) + string.size > bounds.strings) {
            return Status::OutOfRange;
        }
        return Status::Ok;
    }

    /// A Ref names a kind the format has, and a child index only for a child:
    /// every byte of a record has one meaning, so a stray value is refused
    /// rather than ignored.
    [[nodiscard]]
    auto check_ref(Ref const &ref, Bounds const &bounds) -> Status {
        switch (ref.kind) {
            case RefKind::Parent:
            case RefKind::Self: {
                if (ref.child != 0) {
                    return Status::InvArgs;
                }
                return Status::Ok;
            }
            case RefKind::Child: {
                if (ref.child >= bounds.children) {
                    return Status::OutOfRange;
                }
                return Status::Ok;
            }
        }
        return Status::InvArgs;
    }

    /// Known flags only, and rights only on a record that states them.
    [[nodiscard]]
    auto check_rights(RecordFlags const flags, Rights const rights) -> Status {
        if ((flags & ~RecordFlags::All) != RecordFlags::None) {
            return Status::InvArgs;
        }
        if ((flags & RecordFlags::RightsStated) == RecordFlags::None) {
            if (rights != Rights::None) {
                return Status::InvArgs;
            }
            return Status::Ok;
        }
        if ((rights & ~Rights::All) != Rights::None) {
            return Status::InvArgs;
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto check(format::Program const &raw, Bounds const &bounds) -> Status {
        if (raw.runner != Runner::Elf || raw.reserved != 0) {
            return Status::InvArgs;
        }
        return check_string(raw.binary, bounds);
    }

    [[nodiscard]]
    auto check(format::Arg const &raw, Bounds const &bounds) -> Status {
        return check_string(raw.value, bounds);
    }

    [[nodiscard]]
    auto check(format::Child const &raw, Bounds const &bounds) -> Status {
        if (raw.startup != StartupMode::Eager || raw.reserved != 0) {
            return Status::InvArgs;
        }
        return first_error({check_string(raw.name, bounds), check_string(raw.url, bounds)});
    }

    [[nodiscard]]
    auto check(format::Capability const &raw, Bounds const &bounds) -> Status {
        if (raw.reserved != 0) {
            return Status::InvArgs;
        }
        return first_error({check_kind(raw.kind), check_string(raw.name, bounds)});
    }

    [[nodiscard]]
    auto check(format::Use const &raw, Bounds const &bounds) -> Status {
        return first_error({
            check_kind(raw.kind),
            check_rights(raw.flags, raw.rights),
            check_ref(raw.source, bounds),
            check_string(raw.source_name, bounds),
            check_string(raw.target_path, bounds),
            check_string(raw.subdir, bounds),
        });
    }

    [[nodiscard]]
    auto check(format::Offer const &raw, Bounds const &bounds) -> Status {
        return first_error({
            check_kind(raw.kind),
            check_rights(raw.flags, raw.rights),
            check_ref(raw.source, bounds),
            check_string(raw.source_name, bounds),
            check_ref(raw.target, bounds),
            check_string(raw.target_name, bounds),
            check_string(raw.subdir, bounds),
        });
    }

    [[nodiscard]]
    auto check(format::Expose const &raw, Bounds const &bounds) -> Status {
        if (raw.reserved != 0) {
            return Status::InvArgs;
        }
        return first_error({
            check_kind(raw.kind),
            check_ref(raw.source, bounds),
            check_string(raw.source_name, bounds),
            check_ref(raw.target, bounds),
            check_string(raw.target_name, bounds),
        });
    }

    /// Checks the |count| records of type |Raw| from |offset|, and answers the
    /// first refusal.
    template <typename Raw>
    [[nodiscard]]
    auto check_table(u8 const *data, usize const offset, u32 const count, Bounds const &bounds)
        -> Status {
        for (u32 index = 0; index < count; ++index) {
            auto const status =
                check(read<Raw>(data, offset + static_cast<usize>(index) * sizeof(Raw)), bounds);
            if (status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto rights_of(RecordFlags const flags, Rights const rights) -> ustl::Option<Rights> {
        if ((flags & RecordFlags::RightsStated) == RecordFlags::None) {
            return ustl::NONE;
        }
        return rights;
    }
} // namespace

auto Decl::decode(std::span<u8 const> const data) -> Result {
    if (data.data() == nullptr || data.size() < sizeof(format::Header)) {
        return ustl::err(Status::InvArgs);
    }
    auto const *const bytes = data.data();
    auto const header = read<format::Header>(bytes, 0);
    if (header.magic != format::MAGIC || header.reserved != 0 || header.reserved2 != 0) {
        return ustl::err(Status::InvArgs);
    }
    if (header.version != format::VERSION) {
        return ustl::err(Status::NotSupported);
    }
    if (header.total_size != data.size() || header.program_count > 1 ||
        (header.program_count == 0 && header.arg_count != 0)) {
        return ustl::err(Status::InvArgs);
    }

    // Each table starts where the one before ends. Every count is 32 bits
    // and every record at most 56 bytes, so no sum below can wrap a 64-bit
    // usize; whether the result is the size of the bytes is checked after.
    auto offset = static_cast<usize>(sizeof(format::Header));
    auto const next = [&offset](u32 const count, usize const record_size) {
        auto const start = offset;
        offset += static_cast<usize>(count) * record_size;
        return start;
    };
    auto layout = Layout{};
    layout.programs = next(header.program_count, sizeof(format::Program));
    layout.args = next(header.arg_count, sizeof(format::Arg));
    layout.children = next(header.child_count, sizeof(format::Child));
    layout.capabilities = next(header.capability_count, sizeof(format::Capability));
    layout.uses = next(header.use_count, sizeof(format::Use));
    layout.offers = next(header.offer_count, sizeof(format::Offer));
    layout.exposes = next(header.expose_count, sizeof(format::Expose));
    layout.strings = next(header.string_table_size, 1);
    layout.end = offset;
    if (layout.end != data.size()) {
        return ustl::err(Status::InvArgs);
    }

    auto const bounds = Bounds{.strings = header.string_table_size, .children = header.child_count};
    auto const status = first_error({
        check_table<format::Program>(bytes, layout.programs, header.program_count, bounds),
        check_table<format::Arg>(bytes, layout.args, header.arg_count, bounds),
        check_table<format::Child>(bytes, layout.children, header.child_count, bounds),
        check_table<format::Capability>(bytes, layout.capabilities, header.capability_count,
                                        bounds),
        check_table<format::Use>(bytes, layout.uses, header.use_count, bounds),
        check_table<format::Offer>(bytes, layout.offers, header.offer_count, bounds),
        check_table<format::Expose>(bytes, layout.exposes, header.expose_count, bounds),
    });
    if (status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(Decl(bytes, header, layout));
}

auto Decl::program() const -> ustl::Option<Program> {
    if (m_header.program_count == 0) {
        return ustl::NONE;
    }
    auto const raw = read<format::Program>(m_data, m_layout.programs);
    return Program{.runner = raw.runner, .binary = m_strings.text(raw.binary)};
}

auto Decl::args() const -> Records<std::string_view, format::Arg> {
    return records<std::string_view, format::Arg>(m_layout.args, m_header.arg_count);
}

auto Decl::children() const -> Records<Child, format::Child> {
    return records<Child, format::Child>(m_layout.children, m_header.child_count);
}

auto Decl::capabilities() const -> Records<Capability, format::Capability> {
    return records<Capability, format::Capability>(m_layout.capabilities,
                                                   m_header.capability_count);
}

auto Decl::uses() const -> Records<Use, format::Use> {
    return records<Use, format::Use>(m_layout.uses, m_header.use_count);
}

auto Decl::offers() const -> Records<Offer, format::Offer> {
    return records<Offer, format::Offer>(m_layout.offers, m_header.offer_count);
}

auto Decl::exposes() const -> Records<Expose, format::Expose> {
    return records<Expose, format::Expose>(m_layout.exposes, m_header.expose_count);
}

namespace detail {
    auto view(format::Arg const &raw, Strings const &strings) -> std::string_view {
        return strings.text(raw.value);
    }

    auto view(format::Child const &raw, Strings const &strings) -> Child {
        return {
            .name = strings.text(raw.name),
            .url = strings.text(raw.url),
            .startup = raw.startup,
        };
    }

    auto view(format::Capability const &raw, Strings const &strings) -> Capability {
        return {.kind = raw.kind, .name = strings.text(raw.name)};
    }

    auto view(format::Use const &raw, Strings const &strings) -> Use {
        return {
            .kind = raw.kind,
            .source = raw.source,
            .source_name = strings.text(raw.source_name),
            .target_path = strings.text(raw.target_path),
            .subdir = strings.text(raw.subdir),
            .rights = rights_of(raw.flags, raw.rights),
        };
    }

    auto view(format::Offer const &raw, Strings const &strings) -> Offer {
        return {
            .kind = raw.kind,
            .source = raw.source,
            .source_name = strings.text(raw.source_name),
            .target = raw.target,
            .target_name = strings.text(raw.target_name),
            .subdir = strings.text(raw.subdir),
            .rights = rights_of(raw.flags, raw.rights),
        };
    }

    auto view(format::Expose const &raw, Strings const &strings) -> Expose {
        return {
            .kind = raw.kind,
            .source = raw.source,
            .source_name = strings.text(raw.source_name),
            .target = raw.target,
            .target_name = strings.text(raw.target_name),
        };
    }
} // namespace detail
} // namespace component
