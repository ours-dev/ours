/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ort/startup_handles.hpp>

namespace ort {
StartupHandles::StartupHandles(std::span<ours::HandleValue> const handles,
                               std::span<u32> const infos)
    : m_handles(handles), m_infos(infos) {}

/// A taken slot has both halves zeroed. The info alone would do -- no valid
/// handle_info is 0, since type 0 is not a type -- but clearing the value
/// as well means a slot that was taken can never hand its handle out a
/// second time, whatever is later asked for.
auto StartupHandles::take(ours::HandleType const type, u16 const arg) -> ours::HandleValue {
    auto const wanted = ours::handle_info(type, arg);
    auto const count = m_handles.size() < m_infos.size() ? m_handles.size() : m_infos.size();
    for (usize i = 0; i < count; ++i) {
        if (m_infos[i] == wanted && m_handles[i] != ours::HANDLE_INVALID) {
            auto const handle = m_handles[i];
            m_handles[i] = ours::HANDLE_INVALID;
            m_infos[i] = 0;
            return handle;
        }
    }
    return ours::HANDLE_INVALID;
}
} // namespace ort
