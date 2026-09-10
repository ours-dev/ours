#pragma once

#include <gktl/types.hpp>

#include <ustl/views/string_view.hpp>

/// The compiler's own spelling of the enclosing function. Instantiated inside a
/// function template it names the template argument, which is the only way to
/// recover a type's name in this kernel: it is built -fno-rtti, so there is no
/// `typeid` to ask.
///
/// Deliberately without a fallback. This used to be an optional macro that
/// nothing in the tree ever defined, and the `#else` branch handed back an
/// empty name -- so `gktl::magic<T>()` was the hash of "" for every T, every
/// `Canary::verify<T>()` in the kernel compared zero against zero, and the
/// whole mechanism reported nothing while looking like it worked. A compiler
/// that cannot spell its own signature should fail the build rather than
/// quietly disarm every canary in the system.
#if defined(__clang__) || defined(__GNUC__)
#define OURS_PRETTY_FUNCTION __PRETTY_FUNCTION__
#else
#error "gktl::TypeName: no function-signature macro is known for this compiler"
#endif

namespace gktl {
namespace priv {
    template <typename Type>
    FORCE_INLINE [[nodiscard]]
    constexpr auto pretty_function() noexcept -> std::string_view {
        return OURS_PRETTY_FUNCTION;
    }

    /// Cut the type out of the signature.
    ///
    /// The two compilers this builds with spell it differently:
    ///
    ///   clang: "auto gktl::priv::pretty_function() [Type = ours::mm::VmObjectPaged]"
    ///   gcc:   "... pretty_function() [with Type = ours::mm::VmObjectPaged; ...]"
    ///
    /// Both put the answer immediately after "Type = ". GCC then continues with
    /// a ';' and expansions of its own, so the end is the first ';' if there is
    /// one and the final ']' otherwise.
    ///
    /// A signature matching neither shape yields the whole string rather than
    /// nothing. It is still distinct per type, which is all a canary asks for,
    /// and an over-long name is a far better failure than the empty one that
    /// made every magic collide.
    template <typename Type>
    FORCE_INLINE [[nodiscard]]
    constexpr auto strip_type_name() noexcept -> std::string_view {
        constexpr auto PREFIX = std::string_view{"Type = "};

        auto const signature = pretty_function<Type>();
        auto const prefix_at = signature.find(PREFIX);
        if (prefix_at == std::string_view::npos) {
            return signature;
        }

        auto const begin = prefix_at + PREFIX.size();
        auto end = signature.find(';', begin);
        if (end == std::string_view::npos) {
            end = signature.find_last_of(']');
        }
        if (end == std::string_view::npos || end <= begin) {
            return signature;
        }

        return signature.substr(begin, end - begin);
    }
} // namespace priv

template <typename Type, typename = void>
struct TypeName final {
    FORCE_INLINE [[nodiscard]]
    constexpr static auto value() noexcept -> std::string_view {
        return priv::strip_type_name<Type>();
    }

    FORCE_INLINE [[nodiscard]]
    constexpr operator ustl::views::StringView() const noexcept {
        return value();
    }
};
} // namespace gktl
