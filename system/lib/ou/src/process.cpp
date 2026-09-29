/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/process.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto process_exit(i64 const retcode) -> void {
    ours_process_exit(retcode);
}

auto process_create(ours::HandleValue const job, Str const name, ours::HandleValue *const process,
                    ours::HandleValue *const root_vma) -> ours::Status {
    return ours_process_create(job, name.data(), name.size(), ours::ProcessCreateOptions::None,
                               process, root_vma);
}

auto process_spawn_boot(ours::HandleValue const job, Str const name,
                        ours::HandleValue const bootstrap, ours::HandleValue *const root_vma)
    -> ours::Status {
    return ours_process_spawn_boot(job, name.data(), name.size(), bootstrap, root_vma);
}
} // namespace ou
