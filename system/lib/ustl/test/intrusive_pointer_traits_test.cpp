/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>

#include <ustl/collections/intrusive/pointer_rebind.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace {
    template <typename T>
    class FancyPointer final {
    public:
        using ElementType = T;
        using DifferenceType = long;

        template <typename U>
        using Rebind = FancyPointer<U>;

        constexpr FancyPointer() noexcept = default;

        constexpr explicit FancyPointer(T *const ptr) noexcept : m_pointer(ptr) {}

        [[nodiscard]] static constexpr auto pointer_to(T &value) noexcept -> FancyPointer {
            return FancyPointer(&value);
        }

        [[nodiscard]] constexpr auto operator->() const noexcept -> T * {
            return m_pointer;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return m_pointer != nullptr;
        }

    private:
        T *m_pointer = nullptr;
    };

    class CustomPointer final {
    public:
        using ElementType = int;
        using DifferenceType = short;
        using Size = unsigned short;
        using RefMut = int &;

        template <typename T>
        using Rebind = FancyPointer<T>;

        constexpr CustomPointer() noexcept = default;

        constexpr explicit CustomPointer(int *const ptr) noexcept : m_pointer(ptr) {}

        [[nodiscard]] static constexpr auto pointer_to(int &value) noexcept -> CustomPointer {
            return CustomPointer(&value);
        }

        [[nodiscard]] constexpr auto operator->() const noexcept -> int * {
            return m_pointer;
        }

    private:
        int *m_pointer = nullptr;
    };

    struct Base {
        virtual ~Base() = default;
    };

    struct Derived final : Base {};
} // namespace

using namespace ustl::collections::intrusive;

static_assert(std::same_as<PointerRebind<int *, long>::Type, long *>);
static_assert(std::same_as<PointerRebind<FancyPointer<int>, long>::Type, FancyPointer<long>>);
static_assert(std::same_as<PointerTraits<int *>::ElementType, int>);
static_assert(std::same_as<PointerTraits<FancyPointer<int>>::DifferenceType, long>);
static_assert(std::same_as<PointerTraits<void *>::Rebind<int>, int *>);
static_assert(std::same_as<PointerRebind<CustomPointer, long>::Type, FancyPointer<long>>);
static_assert(std::same_as<PointerTraits<CustomPointer>::ElementType, int>);
static_assert(std::same_as<PointerTraits<CustomPointer>::DifferenceType, short>);
static_assert(std::same_as<PointerTraits<CustomPointer>::Size, unsigned short>);
static_assert(std::same_as<PointerTraits<CustomPointer>::RefMut, int &>);

auto main() -> int {
    int value = 7;
    const auto raw = PointerTraits<int *>::pointer_to(value);
    const auto fancy = PointerTraits<FancyPointer<int>>::pointer_to(value);
    if (raw != &value || PointerTraits<FancyPointer<int>>::to_raw_pointer(fancy) != &value) {
        return 1;
    }
    const auto custom = PointerTraits<CustomPointer>::pointer_to(value);
    if (PointerTraits<CustomPointer>::to_raw_pointer(custom) != &value) {
        return 2;
    }

    Derived derived{};
    Base *const base = &derived;
    if (PointerTraits<Derived *>::dynamic_cast_from(base) != &derived) {
        return 3;
    }
    return 0;
}
