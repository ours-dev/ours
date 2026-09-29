/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_OBSERVABLE_HPP
#define EFTL_ELF_OBSERVABLE_HPP 1

#include <eftl/elf/config.hpp>
#include <eftl/elf/traits.hpp>

#include <type_traits>

namespace eftl::elf {
struct EflTraits32;
struct EflTraits64;

/// Base helper class implementing CRTP to implicitly propagate `Traits` template parameters.
///
/// This base class enables derived classes to reuse type traits without explicitly specifying
/// `Traits` template parameters through static polymorphism via the Curiously Recurring Template Pattern.
///
/// #### Typical Usage
/// Derived classes should inherit via CRTP:
/// ```cpp
/// class Segment32
///     : public Object<Class32, Derived>
/// { ... };
/// class Segment64
///     : public Object<Class64, Derived>
/// { ... };
///
/// using ElfView = View<Traits32>;
/// ElfView view;
/// ElfView::Segment segment;
/// decode_segments(view.binary(), view.segments(), Segment.make_observer<LoadSegObserver>());
///
/// // If no this, we must explicitly specify the class of ELF traits, like the following showed,
/// decode_segments(view.binary(), view.segments(), LoadSegObserver<Traits32>(segment));
/// // or
/// decode_segments(view.binary(), view.segments(), LoadSegObserver<ElfView::Traits>(segment));
/// ```
///
/// #### Design Motivation
/// - **Type Erasure Avoidance**: Eliminates runtime overhead from type erasure
/// - **Template Parameter Simplification**: Reduces boilerplate of passing `Traits` explicitly
/// - **Static Polymorphism**: Enables efficient compile-time method dispatch
///
/// @tparam Derived CRTP-derived class type
template <typename Traits, typename Derived>
struct Observable {
    static_assert(std::is_same_v<Traits, Traits32> || std::is_same_v<Traits, Traits64>,
                  "Unknown elf traits type");

    template <template <typename TraitsT> typename Observer>
    [[nodiscard]]
    inline constexpr auto make_observer() -> Observer<Traits> {
        static_assert(std::is_base_of_v<Observable, Derived>,
                      "`Derived` must inherit from `Observable`");

        return {*static_cast<Derived *>(this)};
    }
};

} // namespace eftl::elf

#endif // EFTL_ELF_OBSERVABLE_HPP
