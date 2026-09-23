/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/handle.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto handle_close(ours::HandleValue const handle) -> ours::Status {
    return ours_handle_close(handle);
}
} // namespace ou
