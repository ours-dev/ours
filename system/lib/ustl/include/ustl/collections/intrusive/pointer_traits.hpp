/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_POINTER_TRAITS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_POINTER_TRAITS_HPP 1

#include <concepts>
#include <memory>
#include <type_traits>

#include <cstddef>

#include <ustl/collections/intrusive/pointer_rebind.hpp>

namespace ustl::collections::intrusive {
namespace detail {
    struct VoidReference final {};

    template <typename T>
    using PointerReference = std::conditional_t<std::is_void_v<T>, VoidReference, T> &;

    template <typename PtrMut, typename = void>
    struct PointerElementType final {
        using Type = typename std::pointer_traits<PtrMut>::element_type;
    };

    template <typename PtrMut>
    struct PointerElementType<PtrMut, std::void_t<typename PtrMut::ElementType>> final {
        using Type = typename PtrMut::ElementType;
    };

    template <typename PtrMut, typename = void>
    struct PointerDifferenceType final {
        using Type = typename std::pointer_traits<PtrMut>::difference_type;
    };

    template <typename PtrMut>
    struct PointerDifferenceType<PtrMut, std::void_t<typename PtrMut::DifferenceType>> final {
        using Type = typename PtrMut::DifferenceType;
    };

    template <typename PtrMut, typename Difference, typename = void>
    struct PointerSizeType final {
        using Type = std::make_unsigned_t<Difference>;
    };

    template <typename PtrMut, typename Difference>
    struct PointerSizeType<PtrMut, Difference, std::void_t<typename PtrMut::Size>> final {
        using Type = typename PtrMut::Size;
    };

    template <typename PtrMut, typename Element, typename = void>
    struct PointerReferenceType final {
        using Type = PointerReference<Element>;
    };

    template <typename PtrMut, typename Element>
    struct PointerReferenceType<PtrMut, Element, std::void_t<typename PtrMut::RefMut>> final {
        using Type = typename PtrMut::RefMut;
    };
} // namespace detail

template <typename TPtrMut>
struct PointerTraits {
    using PtrMut = TPtrMut;
    using ElementType = typename detail::PointerElementType<TPtrMut>::Type;
    using DifferenceType = typename detail::PointerDifferenceType<TPtrMut>::Type;
    using Size = typename detail::PointerSizeType<TPtrMut, DifferenceType>::Type;
    using RefMut = typename detail::PointerReferenceType<TPtrMut, ElementType>::Type;

    template <typename T>
    using Rebind = typename PointerRebind<TPtrMut, T>::Type;

    template <typename T>
    struct RebindPointer final {
        using Type = Rebind<T>;
    };

    [[nodiscard]]
    static constexpr auto pointer_to(RefMut ref) noexcept -> PtrMut
        requires(!std::is_void_v<ElementType>)
    {
        if constexpr (requires(RefMut value) {
                          { PtrMut::pointer_to(value) } -> std::convertible_to<PtrMut>;
                      }) {
            return PtrMut::pointer_to(ref);
        } else {
            return PtrMut(std::addressof(ref));
        }
    }

    template <typename T>
    [[nodiscard]]
    static constexpr auto to_raw_pointer(T *const ptr) noexcept -> T * {
        return ptr;
    }

    template <typename OtherPointer>
        requires(!std::is_pointer_v<OtherPointer>)
    [[nodiscard]]
    static constexpr auto to_raw_pointer(const OtherPointer &ptr) noexcept
        -> decltype(to_raw_pointer(ptr.operator->())) {
        return to_raw_pointer(ptr.operator->());
    }

    template <typename OtherPointer>
    [[nodiscard]]
    static constexpr auto static_cast_from(const OtherPointer &ptr) noexcept -> PtrMut
        requires(!std::is_void_v<ElementType>)
    {
        if constexpr (requires(const OtherPointer &value) {
                          { PtrMut::static_cast_from(value) } -> std::convertible_to<PtrMut>;
                      }) {
            return PtrMut::static_cast_from(ptr);
        } else {
            const auto raw_pointer = to_raw_pointer(ptr);
            return raw_pointer == nullptr ? PtrMut{}
                                          : pointer_to(*static_cast<ElementType *>(raw_pointer));
        }
    }

    template <typename OtherPointer>
    [[nodiscard]]
    static constexpr auto const_cast_from(const OtherPointer &ptr) noexcept -> PtrMut
        requires(!std::is_void_v<ElementType>)
    {
        if constexpr (requires(const OtherPointer &value) {
                          { PtrMut::const_cast_from(value) } -> std::convertible_to<PtrMut>;
                      }) {
            return PtrMut::const_cast_from(ptr);
        } else {
            const auto raw_pointer = to_raw_pointer(ptr);
            return raw_pointer == nullptr ? PtrMut{}
                                          : pointer_to(const_cast<ElementType &>(*raw_pointer));
        }
    }

    template <typename OtherPointer>
    [[nodiscard]]
    static constexpr auto dynamic_cast_from(const OtherPointer &ptr) noexcept -> PtrMut
        requires(!std::is_void_v<ElementType>)
    {
        if constexpr (requires(const OtherPointer &value) {
                          { PtrMut::dynamic_cast_from(value) } -> std::convertible_to<PtrMut>;
                      }) {
            return PtrMut::dynamic_cast_from(ptr);
        } else {
            const auto raw_pointer = dynamic_cast<ElementType *>(to_raw_pointer(ptr));
            return raw_pointer == nullptr ? PtrMut{} : pointer_to(*raw_pointer);
        }
    }
};

template <typename T>
struct PointerTraits<T *> {
    using PtrMut = T *;
    using ElementType = T;
    using DifferenceType = std::ptrdiff_t;
    using Size = std::size_t;
    using RefMut = detail::PointerReference<ElementType>;

    template <typename U>
    using Rebind = U *;

    template <typename U>
    struct RebindPointer final {
        using Type = Rebind<U>;
    };

    [[nodiscard]]
    static constexpr auto pointer_to(RefMut ref) noexcept -> PtrMut
        requires(!std::is_void_v<ElementType>)
    {
        return std::addressof(ref);
    }

    [[nodiscard]]
    static constexpr auto to_raw_pointer(PtrMut const ptr) noexcept -> PtrMut {
        return ptr;
    }

    template <typename U>
    [[nodiscard]]
    static constexpr auto static_cast_from(U *const ptr) noexcept -> PtrMut {
        return static_cast<PtrMut>(ptr);
    }

    template <typename U>
    [[nodiscard]]
    static constexpr auto const_cast_from(U *const ptr) noexcept -> PtrMut {
        return const_cast<PtrMut>(ptr);
    }

    template <typename U>
    [[nodiscard]]
    static constexpr auto dynamic_cast_from(U *const ptr) noexcept -> PtrMut {
        return dynamic_cast<PtrMut>(ptr);
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_POINTER_TRAITS_HPP
