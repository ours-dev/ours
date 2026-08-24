/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_LINK_HPP
#define EFTL_ELF_LINK_HPP 1

#include <eftl/elf/config.hpp>

#include <limits>
#include <utility>

namespace eftl::elf {
template <typename Addr, typename SAddr>
[[nodiscard]]
inline constexpr auto add_bias(Addr bias, SAddr addend, Addr &value) -> bool {
    if (addend >= 0) {
        auto const positive_addend = static_cast<Addr>(addend);
        if (bias > std::numeric_limits<Addr>::max() - positive_addend) {
            return false;
        }
        value = bias + positive_addend;
        return true;
    }
    auto const magnitude = static_cast<Addr>(-(addend + 1)) + 1;
    if (bias < magnitude) {
        return false;
    }
    value = bias - magnitude;
    return true;
}

template <typename Memory, typename Relocations>
[[nodiscard]]
inline constexpr auto apply_relative_record(Memory &memory, typename Relocations::Addr bias,
                                            typename Relocations::RelativeRelocation relocation)
    -> bool {
    using Addr = typename Relocations::Addr;
    Addr value{};
    if (relocation.addend_in_place) {
        auto *addend = memory.template read_at<Addr>(relocation.offset);
        if (addend == nullptr || *addend > std::numeric_limits<Addr>::max() - bias) {
            return false;
        }
        value = *addend + bias;
    } else if (!add_bias(bias, relocation.addend, value)) {
        return false;
    }
    return memory.template write<Addr>(relocation.offset, value);
}

template <typename Memory, typename Relocations>
[[nodiscard]]
inline constexpr auto relocate_relative(Memory &&memory, Relocations &relocations,
                                        typename Relocations::Addr bias) -> bool {
    return relocations.visit_relative([&memory, bias](auto relocation) -> bool {
        return apply_relative_record<Memory, Relocations>(memory, bias, relocation);
    });
}

template <typename Memory, typename Relocations, typename IsRelative>
[[nodiscard]]
inline constexpr auto relocate_relative(Memory &&memory, Relocations &relocations,
                                        typename Relocations::Addr bias, IsRelative &&is_relative)
    -> bool {
    return relocations.visit_relative(
        std::forward<IsRelative>(is_relative), [&memory, bias](auto relocation) -> bool {
            return apply_relative_record<Memory, Relocations>(memory, bias, relocation);
        });
}

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_LINK_HPP
