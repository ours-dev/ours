/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_DYNAMIC_HPP
#define EFTL_ELF_DYNAMIC_HPP 1

#include <eftl/elf/details/dyn.hpp>

#include <array>
#include <limits>
#include <memory>
#include <type_traits>

#include <ustl/option.hpp>
#include <ustl/traits/integral_constant.hpp>
#include <ustl/util/type_map.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
template <DynTag>
struct DynTagMatcher {};

template <typename Observer, DynTag... Tags>
struct DynamicObserver {};

template <typename Memory, typename Dyn, typename Observer, DynTag... Tags>
[[nodiscard]]
auto decode_dynamic_at(Memory &&memory, Dyn const &entry,
                       DynamicObserver<Observer, Tags...> &observer) -> bool {
    auto is_ok = true;
    auto invoke_observe = [&](auto const &tag) -> bool {
        is_ok &= static_cast<Observer &>(observer).observe(memory, entry.d_val, tag);
        return true;
    };

    ((DynTag(entry.d_tag) == Tags && invoke_observe(DynTagMatcher<Tags>())) || ...);
    return is_ok;
}

template <typename Memory, typename Range, typename... Observer>
[[nodiscard]]
auto decode_dynamic(Memory &&memory, Range const &dynamics, Observer &&...observers) -> bool {
    for (auto const dyn: dynamics) {
        if (DynTag(dyn.d_tag) == DynTag::DtNull) {
            return (observers.finish(memory) && ...);
        }

        if ((!decode_dynamic_at(memory, dyn, observers) || ...)) {
            return false;
        }
    }
    return false;
}

template <typename Observer, typename Info, typename Traits, typename TagToIndex, DynTag... Tags>
struct DynInfoObserver: public DynamicObserver<Observer, Tags...> {
    using Addr = typename Traits::Addr;
    using InfoType = Info;
    using TagIndexMap = TagToIndex;

    template <DynTag Tag>
    static usize const INDEX_V = ustl::TypeAlgos::Find<TagIndexMap, DynTagMatcher<Tag>>();

    DynInfoObserver(Info &info) : m_info(std::addressof(info)), m_caches(), m_seen() {}

    FORCE_INLINE [[nodiscard]]
    constexpr auto info() -> InfoType & {
        return *m_info;
    }

    template <DynTag Tag>
    FORCE_INLINE [[nodiscard]]
    constexpr auto has() const noexcept -> bool {
        return m_seen[INDEX_V<Tag>];
    }

    template <typename Memory, DynTag Tag>
    FORCE_INLINE [[nodiscard]]
    constexpr auto observe(Memory &&memory, Addr val, DynTagMatcher<Tag>) -> bool {
        (void)memory;
        static_assert(!std::is_same_v<ustl::TypeAlgos::Find<TagIndexMap, DynTagMatcher<Tag>>,
                                      ustl::TypeAlgos::NotFound>,
                      "Unsupported tag");
        m_caches[INDEX_V<Tag>] = val;
        m_seen[INDEX_V<Tag>] = true;
        return true;
    }

    template <typename T, auto Setter, DynTag PosTag, DynTag SizeTag,
              DynTag CountTag = DynTag::Unknown, typename Memory>
    [[nodiscard]]
    auto write_back(Memory &&memory) -> bool {
        if (!m_seen[INDEX_V<PosTag>] && !m_seen[INDEX_V<SizeTag>]) {
            return true;
        }
        if (!m_seen[INDEX_V<PosTag>] || !m_seen[INDEX_V<SizeTag>]) {
            return false;
        }
        Addr const pos = m_caches[INDEX_V<PosTag>], size_bytes = m_caches[INDEX_V<SizeTag>];
        auto const native_pos = to_usize(pos);
        auto const native_size = to_usize(size_bytes);
        if (native_pos.is_none() || native_size.is_none() || (pos % alignof(T)) != 0 ||
            native_size.value() % sizeof(T) != 0 || !fits_memory_range<Memory>(pos, size_bytes)) {
            return false;
        }

        Addr const count = size_bytes / sizeof(T);
        auto const native_count = to_usize(count);
        if (native_count.is_none() ||
            native_count.value() > std::numeric_limits<usize>::max() / sizeof(T) ||
            native_count.value() * sizeof(T) != native_size.value()) {
            return false;
        }

        usize requested_count = native_count.value();
        if constexpr (CountTag != DynTag::Unknown) {
            auto const count_tag = [&]() -> ustl::Option<usize> {
                if (m_seen[INDEX_V<CountTag>]) {
                    return to_usize(m_caches[INDEX_V<CountTag>]);
                }
                return native_count;
            }();
            if (count_tag.is_none() || count_tag.value() > native_count.value()) {
                return false;
            }
            requested_count = count_tag.value();
        }

        auto const consume_table = [&](auto table) -> bool {
            if (table.size() != native_count.value()) {
                return false;
            }
            if constexpr (CountTag != DynTag::Unknown) {
                (m_info->*Setter)(table, requested_count);
            } else {
                (m_info->*Setter)(table);
            }
            return true;
        };
        if constexpr (requires { typename std::remove_cvref_t<Memory>::Addr; }) {
            using MemoryAddr = typename std::remove_cvref_t<Memory>::Addr;
            return consume_table(
                memory.template read_at_n<T>(static_cast<MemoryAddr>(pos), native_count.value()));
        } else {
            return consume_table(
                memory.template read_at_n<T>(native_pos.value(), native_count.value()));
        }
    }

    template <DynTag Tag>
    FORCE_INLINE [[nodiscard]]
    constexpr auto at() -> Addr {
        return m_caches[INDEX_V<Tag>];
    }

  private:
    template <typename Value>
    [[nodiscard]]
    static constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    template <typename Memory>
    [[nodiscard]]
    static constexpr auto fits_memory_range(Addr address, Addr size_bytes) noexcept -> bool {
        if (size_bytes != 0 && address > std::numeric_limits<Addr>::max() - size_bytes) {
            return false;
        }
        if constexpr (requires { typename std::remove_cvref_t<Memory>::Addr; }) {
            using MemoryAddr = typename std::remove_cvref_t<Memory>::Addr;
            static_assert(std::is_integral_v<MemoryAddr> && std::is_unsigned_v<MemoryAddr>);
            if constexpr (sizeof(Addr) > sizeof(MemoryAddr)) {
                auto const memory_max = static_cast<Addr>(std::numeric_limits<MemoryAddr>::max());
                return address <= memory_max &&
                       (size_bytes == 0 || size_bytes - 1 <= memory_max - address);
            }
        }
        return true;
    }

    Info *m_info;
    std::array<Addr, sizeof...(Tags)> m_caches{};
    std::array<bool, sizeof...(Tags)> m_seen{};
};

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_DYNAMIC_HPP
