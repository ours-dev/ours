/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/process.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto process_spawn_boot(ours::HandleValue const job, Str const name,
                        ours::HandleValue const bootstrap) -> ours::Status {
    return ours_process_spawn_boot(job, name.data(), name.size(), bootstrap);
}
} // namespace ou
