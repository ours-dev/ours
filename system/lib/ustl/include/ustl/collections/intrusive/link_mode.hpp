/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_LINK_MODE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_LINK_MODE_HPP 1

#include <type_traits>

namespace ustl::collections::intrusive {
    enum class LinkModeType {
        NormalLink,
        SafeLink,
        AutoUnlink,
    };

    template <LinkModeType Mode>
    struct IsSafeAutounlink final
        : std::bool_constant<Mode == LinkModeType::SafeLink || Mode == LinkModeType::AutoUnlink> {
        static constexpr bool VALUE = Mode == LinkModeType::SafeLink || Mode == LinkModeType::AutoUnlink;
    };

    template <LinkModeType Mode>
    struct LinkMode final {
        template <typename Base>
        struct Pack : Base {
            static constexpr LinkModeType LINK_MODE = Mode;
        };
    };

    using AutoUnlink = LinkMode<LinkModeType::AutoUnlink>;
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_LINK_MODE_HPP
