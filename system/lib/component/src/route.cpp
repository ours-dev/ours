/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The router's non-template half: Subdir, and the lookups and hop rules
/// route() is built from. route() itself is a template over the instance
/// type, so it lives in <component/route.hpp>.
#include <component/route.hpp>

#include <span>
#include <string_view>

#include <cstring>

namespace component {
auto Subdir::prepend(std::string_view const head) -> bool {
    if (head.empty()) {
        return true;
    }
    // The joining '/' is needed only when something is already there.
    auto added = head.size();
    if (m_size != 0) {
        added += 1;
    }
    if (added > m_bytes.size() - m_size) {
        return false;
    }

    auto start = m_bytes.size() - m_size;
    if (m_size != 0) {
        start -= 1;
        m_bytes[start] = '/';
    }
    start -= head.size();
    std::memcpy(m_bytes.data() + start, head.data(), head.size());
    m_size += static_cast<u32>(added);
    return true;
}

auto Subdir::view() const -> std::string_view {
    return {m_bytes.data() + (m_bytes.size() - m_size), m_size};
}

namespace detail {
    auto find_builtin(std::span<Builtin const> const builtins, Kind const kind,
                      std::string_view const name) -> ustl::Option<Builtin> {
        for (auto const &builtin: builtins) {
            if (builtin.kind == kind && builtin.name == name) {
                return builtin;
            }
        }
        return ustl::NONE;
    }

    auto builtin_type(Builtin const &builtin) -> std::string_view {
        // Empty is the name, as in a manifest's Capability.
        if (builtin.type.empty()) {
            return builtin.name;
        }
        return builtin.type;
    }

    auto find_offer(Decl const &decl, Kind const kind, u32 const child, std::string_view const name)
        -> ustl::Option<Offer> {
        for (auto const offer: decl.offers()) {
            auto const to_child =
                offer.target.kind == RefKind::Child && offer.target.child == child;
            if (offer.kind == kind && to_child && offer.target_name == name) {
                return offer;
            }
        }
        return ustl::NONE;
    }

    auto find_expose(Decl const &decl, Kind const kind, std::string_view const name)
        -> ustl::Option<Expose> {
        for (auto const expose: decl.exposes()) {
            auto const to_parent = expose.target.kind == RefKind::Parent;
            if (expose.kind == kind && to_parent && expose.target_name == name) {
                return expose;
            }
        }
        return ustl::NONE;
    }

    auto find_capability(Decl const &decl, Kind const kind, std::string_view const name)
        -> ustl::Option<Capability> {
        for (auto const capability: decl.capabilities()) {
            if (capability.kind == kind && capability.name == name) {
                return capability;
            }
        }
        return ustl::NONE;
    }

    auto open_path_fits(std::string_view const path, std::string_view const subdir) -> bool {
        // The Open carries the path without its leading '/'. The joining '/'
        // is counted whenever there is a subdir (for "/", which the compiler
        // refuses, that is one byte more than the Open needs).
        auto size = path.size();
        if (path.starts_with('/')) {
            size -= 1;
        }
        if (!subdir.empty()) {
            size += 1 + subdir.size();
        }
        return size <= MAX_PATH_LENGTH;
    }

    auto Hops::apply(ustl::Option<Rights> const &rights, std::string_view const subdir)
        -> ours::Status {
        // Held against the last hop that stated rights, not against the use:
        // a hop that offers more than the hop beyond it gave is the mistake,
        // even when what it offers is still enough for the use.
        if (rights.is_some() && m_last.is_some()) {
            auto const kept = rights.value() & m_last.value();
            if (kept != m_last.value()) {
                return ours::Status::AccessDenied;
            }
        }
        // prepend() leaves the Subdir as it was when the result would not fit.
        if (!m_subdir.prepend(subdir)) {
            return ours::Status::NotFound;
        }
        // A hop that states no rights narrows nothing, and the next hop is
        // held against the one before it.
        if (rights.is_some()) {
            m_last = rights;
        }
        return ours::Status::Ok;
    }
} // namespace detail
} // namespace component
