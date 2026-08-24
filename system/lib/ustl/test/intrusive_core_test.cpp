/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>
#include <cstddef>

#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>

namespace {
    struct Defaults {
        using Size = std::size_t;
        [[maybe_unused]] static constexpr bool CONSTANT_TIME_SIZE = true;
    };

    using Config = typename ustl::collections::intrusive::PackOptions<
        Defaults,
        ustl::collections::intrusive::Size<unsigned short>,
        ustl::collections::intrusive::ConstantTimeSize<false>>::Type;
} // namespace

static_assert(std::same_as<Config::Size, unsigned short>);
static_assert(!Config::CONSTANT_TIME_SIZE);
static_assert(ustl::collections::intrusive::IsSafeAutounlink<
              ustl::collections::intrusive::LinkModeType::SafeLink>::VALUE);
static_assert(ustl::collections::intrusive::IsSafeAutounlink<
              ustl::collections::intrusive::LinkModeType::AutoUnlink>::VALUE);
static_assert(!ustl::collections::intrusive::IsSafeAutounlink<
              ustl::collections::intrusive::LinkModeType::NormalLink>::VALUE);

auto main() -> int {
    return 0;
}
