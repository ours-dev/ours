/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The program the fixture manifests name. It is built so that
/// component_manifest() reads a real install name, and never runs.
#include <ort/main.hpp>

auto main([[maybe_unused]] int const argc, [[maybe_unused]] char **const argv) -> int {
    return 0;
}
