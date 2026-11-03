/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <fsio/namespace.hpp>

#include <algorithm>

namespace fsio {
namespace {
    using ours::Status;
    using ours::Uandle;

    constexpr auto ROOT = std::string_view{"/"};

    /// Whether the clean absolute path |inner| lies strictly below the clean
    /// absolute path |outer|: |outer| is "/" and |inner| is anything else, or
    /// |inner| starts with |outer| and a '/'. "/boot/bin" lies below "/boot";
    /// "/bootfs" does not, which a plain prefix test would get wrong.
    auto lies_below(std::string_view const inner, std::string_view const outer) -> bool {
        if (outer == ROOT) {
            return inner != ROOT;
        }
        return inner.size() > outer.size() && inner.starts_with(outer) &&
               inner[outer.size()] == '/';
    }

    /// Whether every name of the clean path |path| fits in
    /// prot::io::MAX_NAME_LENGTH, as the walk down the namespace checks each
    /// name it reaches.
    auto names_fit(std::string_view const path) -> bool {
        auto start = usize{0};
        while (start <= path.size()) {
            auto end = path.find('/', start);
            if (end == std::string_view::npos) {
                end = path.size();
            }
            if (end - start > prot::io::MAX_NAME_LENGTH) {
                return false;
            }
            start = end + 1;
        }
        return true;
    }
} // namespace

auto Namespace::path_of(Binding const &binding) const -> std::string_view {
    return {m_paths.data() + binding.offset, binding.size};
}

auto Namespace::find(std::string_view const path) const -> usize {
    for (auto index = usize{0}; index < m_count; ++index) {
        if (path_of(m_bindings[index]) == path) {
            return index;
        }
    }
    return MAX_BINDINGS;
}

auto Namespace::bind(std::string_view const path, Uandle const directory) -> Status {
    PathBuffer clean;
    if (auto const status = clean_path(path, clean); status != Status::Ok) {
        return status;
    }
    if (directory == Uandle::Invalid) {
        return Status::BadHandle;
    }
    auto const cleaned = clean.view();
    if (!cleaned.starts_with('/')) {
        return Status::InvArgs;
    }
    if (!names_fit(cleaned)) {
        return Status::BadPath;
    }

    // No bound path overlaps another, so |cleaned| is equal to, below or
    // above at most one kind of them, and the order of the checks does not
    // decide the answer.
    for (auto index = usize{0}; index < m_count; ++index) {
        auto const bound = path_of(m_bindings[index]);
        if (bound == cleaned) {
            return Status::AlreadyExists;
        }
        if (lies_below(cleaned, bound)) {
            // Shadowing: the new directory would hide part of |bound|.
            return Status::NotSupported;
        }
        if (lies_below(bound, cleaned)) {
            // The namespace already has a node at |cleaned|, made to hold
            // |bound|. Binding the root over it is an "overlay"; binding any
            // other such node finds it existing.
            if (cleaned == ROOT) {
                return Status::NotSupported;
            }
            return Status::AlreadyExists;
        }
    }

    if (m_count == MAX_BINDINGS || cleaned.size() > BOUND_PATHS_CAPACITY - m_paths_size) {
        return Status::NoResource;
    }
    std::ranges::copy(cleaned, m_paths.begin() + static_cast<std::ptrdiff_t>(m_paths_size));
    m_bindings[m_count] = Binding{
        .directory = directory,
        .offset = static_cast<u16>(m_paths_size),
        .size = static_cast<u16>(cleaned.size()),
    };
    m_count += 1;
    m_paths_size += cleaned.size();
    return Status::Ok;
}

auto Namespace::unbind(std::string_view const path) -> ustl::Result<Uandle, Status> {
    PathBuffer clean;
    if (auto const status = clean_path(path, clean); status != Status::Ok) {
        return ustl::err(status);
    }
    auto const cleaned = clean.view();
    if (!cleaned.starts_with('/')) {
        return ustl::err(Status::InvArgs);
    }
    if (!names_fit(cleaned)) {
        return ustl::err(Status::BadPath);
    }
    if (cleaned != ROOT && find(ROOT) != MAX_BINDINGS) {
        return ustl::err(Status::BadPath);
    }
    auto const index = find(cleaned);
    if (index == MAX_BINDINGS) {
        return ustl::err(Status::NotFound);
    }

    // The paths lie in m_paths in the order of m_bindings, so everything
    // after the one removed moves down by its size, text and offsets alike.
    auto const removed = m_bindings[index];
    auto const paths = m_paths.begin();
    std::ranges::copy(paths + removed.offset + removed.size,
                      paths + static_cast<std::ptrdiff_t>(m_paths_size), paths + removed.offset);
    m_paths_size -= removed.size;
    for (auto next = index + 1; next < m_count; ++next) {
        m_bindings[next - 1] = m_bindings[next];
        m_bindings[next - 1].offset -= removed.size;
    }
    m_count -= 1;
    m_bindings[m_count] = Binding{};
    return ustl::ok(removed.directory);
}

auto Namespace::resolve(PathBuffer const &path) const -> ustl::Result<Resolved, Status> {
    auto const view = path.view();
    if (!view.starts_with('/')) {
        return ustl::err(Status::NotFound);
    }
    if (!names_fit(view)) {
        return ustl::err(Status::BadPath);
    }

    auto above_a_mount_point = false;
    for (auto index = usize{0}; index < m_count; ++index) {
        auto const &binding = m_bindings[index];
        auto const bound = path_of(binding);
        auto const under = bound == view || lies_below(view, bound);
        if (under && binding.closed != Status::Ok) {
            // Its server is gone, and said why once (mark_closed()).
            return ustl::err(binding.closed);
        }
        if (bound == view) {
            return ustl::ok(Resolved{.directory = binding.directory, .rest = "."});
        }
        if (lies_below(view, bound)) {
            // What follows the mount point and the '/' after it; "/" ends
            // in its own '/'.
            auto skip = bound.size() + 1;
            if (bound == ROOT) {
                skip = ROOT.size();
            }
            return ustl::ok(Resolved{.directory = binding.directory, .rest = view.substr(skip)});
        }
        if (lies_below(bound, view)) {
            above_a_mount_point = true;
        }
    }
    // The root is a node even when nothing is bound, so "/" names a node
    // with nothing to connect to rather than nothing at all.
    if (above_a_mount_point || view == ROOT) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::err(Status::NotFound);
}

auto Namespace::entry(usize const index) const -> ustl::Option<Entry> {
    if (index >= m_count) {
        return ustl::NONE;
    }
    auto const &binding = m_bindings[index];
    return ustl::some(Entry{
        .path = path_of(binding),
        .directory = binding.directory,
        .closed = binding.closed,
    });
}

auto Namespace::mark_closed(Uandle const directory, Status const status) -> Status {
    if (status == Status::Ok) {
        return Status::InvArgs;
    }
    for (auto index = usize{0}; index < m_count; ++index) {
        auto &binding = m_bindings[index];
        if (binding.directory != directory) {
            continue;
        }
        if (binding.closed == Status::Ok) {
            binding.closed = status;
        }
        return Status::Ok;
    }
    return Status::NotFound;
}
} // namespace fsio
