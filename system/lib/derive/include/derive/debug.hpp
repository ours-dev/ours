#ifndef DERIVE_DEBUG_HPP
#define DERIVE_DEBUG_HPP 1

namespace derive {
    inline constexpr struct {} debug;
} // namespace derive

template <typename E>
    requires std::is_enum_v<E>
constexpr auto to_string(E value) -> char const * {
    template for (constexpr auto e : std::meta::enumerators_of(^^E)) {
        if (value == [:e:]) {
            return std::meta::identifier_of(e);
        }
    }

    return "<undebugged>";
}

template <typename T>
    requires std::is_class_v<T>
constexpr auto to_string(T const &value) -> char const * {
    template for (constexpr auto e : std::meta::enumerators_of(^^E)) {
        if (value == [:e:]) {
            return std::meta::identifier_of(e);
        }
    }

    return "<undebugged>";
}

#endif // #ifndef DERIVE_DEBUG_HPP