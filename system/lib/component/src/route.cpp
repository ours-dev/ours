/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The router's non-template half. route() itself is a template over the
/// instance type, so it lives in <component/route.hpp>.
#include <component/route.hpp>

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
} // namespace component
