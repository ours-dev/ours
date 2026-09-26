/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The smallest program on the runtime: a main and nothing else, so that
/// linking it proves the start-up code links -- _start as the entry point,
/// main found under its C name, and no PLT. It is never packaged or run;
/// the first programs that run on the runtime are init and the shell, when
/// they move to it (design K14).
#include <runtime/main.hpp>
#include <runtime/startup.hpp>

auto main(int const argc, char **const argv) -> int {
    // Touch what the runtime hands out, so that none of it is optimised away
    // unlinked: the arguments, a startup handle, the vDSO's address.
    auto const handle = runtime::take_startup_handle(ours::HandleType::User0, 0);
    return argc + (argv[0] != nullptr) + static_cast<int>(handle & 1) +
           static_cast<int>(runtime::vdso_base() & 1);
}
