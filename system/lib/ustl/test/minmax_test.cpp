/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <array>
#include <cassert>

#include <ustl/algorithms/minmax.hpp>

static_assert(ustl::algorithms::min(3, 7) == 3);
static_assert(ustl::algorithms::max(3, 7) == 7);
static_assert(ustl::algorithms::clamp(8, 2, 5) == 5);
static_assert(ustl::algorithms::less<int> {}(2, 5));

auto main() -> int {
  constexpr auto values = std::array {9, 1, 6, 4};
  auto const [minimum, maximum] = ustl::algorithms::minmax_element(values.begin(), values.end());
  assert(*minimum == 1);
  assert(*maximum == 9);
}
