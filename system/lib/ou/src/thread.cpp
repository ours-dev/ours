/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/thread.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto thread_create(ours::HandleValue const process, Str const name, ours::HandleValue *const out)
    -> ours::Status {
    return ours_thread_create(process, name.data(), name.size(), ours::ThreadCreateOptions::None,
                              out);
}
} // namespace ou
