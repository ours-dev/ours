/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_POINTER_PLUS_BITS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_POINTER_PLUS_BITS_HPP 1

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/intrusive_fwd.hpp>

namespace ustl::collections::intrusive {
    namespace detail {
        template <std::size_t NumBits>
        [[nodiscard]] consteval auto pointer_bit_mask() noexcept -> std::uintptr_t {
            static_assert(NumBits < std::numeric_limits<std::uintptr_t>::digits);
            if constexpr (NumBits == 0) {
                return 0;
            } else {
                return std::numeric_limits<std::uintptr_t>::max() >>
                       (std::numeric_limits<std::uintptr_t>::digits - NumBits);
            }
        }
    } // namespace detail

    template <typename VoidPointer, std::size_t Alignment>
    struct MaxPointerPlusBits final {
        static constexpr std::size_t VALUE = 0;
    };

    template <std::size_t Alignment>
    struct MaxPointerPlusBits<void *, Alignment> final {
        static_assert(Alignment != 0);

        static constexpr std::size_t VALUE = static_cast<std::size_t>(std::countr_zero(Alignment));
    };

    template <typename T, std::size_t NumBits>
    struct PointerPlusBits<T *, NumBits> final {
        using PtrMut = T *;

        static constexpr std::uintptr_t MASK = detail::pointer_bit_mask<NumBits>();

        [[nodiscard]] static constexpr auto get_pointer(PtrMut const ptr) noexcept -> PtrMut {
            return reinterpret_cast<PtrMut>(reinterpret_cast<std::uintptr_t>(ptr) & ~MASK);
        }

        static constexpr auto set_pointer(PtrMut &ptr, PtrMut const value) noexcept -> void {
            const auto raw_value = reinterpret_cast<std::uintptr_t>(value);
            detail::require((raw_value & MASK) == 0);
            ptr = reinterpret_cast<PtrMut>(raw_value | (reinterpret_cast<std::uintptr_t>(ptr) & MASK));
        }

        [[nodiscard]] static constexpr auto get_bits(PtrMut const ptr) noexcept -> std::size_t {
            return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(ptr) & MASK);
        }

        static constexpr auto set_bits(PtrMut &ptr, std::size_t const bits) noexcept -> void {
            detail::require(bits <= MASK);
            ptr = reinterpret_cast<PtrMut>(reinterpret_cast<std::uintptr_t>(get_pointer(ptr)) | bits);
        }
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_POINTER_PLUS_BITS_HPP
