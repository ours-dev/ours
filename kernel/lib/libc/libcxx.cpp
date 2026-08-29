#include <ours/panic.hpp>

#include <charconv>
#include <exception>
#include <format>
#include <functional>
#include <string>
#include <version>

namespace std {
// Must explicit instanilization
template class basic_string<char>;

exception::~exception() noexcept {}

auto exception::what() const noexcept -> char const * {
    return "ours not exception";
}

#ifdef __GLIBCXX__
__cow_string::__cow_string() {}

__cow_string::~__cow_string() {}
#endif

runtime_error::runtime_error(char const *)
#ifdef _LIBCPP_VERSION
    : __imp_("")
#endif
{
}

runtime_error::runtime_error(string const &msg)
#ifdef _LIBCPP_VERSION
    : __imp_(msg.data())
#endif
{
}

runtime_error::~runtime_error() noexcept {}

auto runtime_error::what() const noexcept -> char const * {
    return "exception: c++ runtime exception";
}

domain_error::~domain_error() {}

auto logic_error::what() const noexcept -> char const * {
    return "exception: c++ logical error";
}

logic_error::logic_error(char const *msg) : __imp_(msg) {}

logic_error::~logic_error() {}

length_error::~length_error() {}

auto __throw_out_of_range_fmt(char const *fmt, ...) -> void {
    ours::panic(fmt);
}

#ifdef _LIBCPP_STD_VER
// _LIBCPP_ABI_NAMESPACE, not a hardcoded __1: this is the same macro libc++'s
// own _LIBCPP_BEGIN_NAMESPACE_STD opens, so the definitions below land in
// whatever inline namespace this build of libc++ actually uses. Spelled __1,
// they opened a namespace that does not enclose std::__libcpp_refstring and
// left std::__ph unfindable.
inline namespace _LIBCPP_ABI_NAMESPACE {
#define TO_CHARS_ATTRS _LIBCPP_AVAILABILITY_TO_CHARS_FLOATING_POINT _LIBCPP_EXPORTED_FROM_ABI

    __libcpp_refstring::__libcpp_refstring(char const *) {}

    __libcpp_refstring::~__libcpp_refstring() {}
#define GLIBCXX_NOEXCEPT
#else
#define TO_CHARS_ATTRS
#define GLIBCXX_NOEXCEPT noexcept
#endif
    // The functions bottom should never be used, because kernel be unable to use float point number.
    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, float __value) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, double __value) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, long double __value) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, float __value,
                             chars_format __fmt) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, double __value,
                             chars_format __fmt) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, long double __value,
                             chars_format __fmt) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, float __value, chars_format __fmt,
                             int __precision) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, double __value, chars_format __fmt,
                             int __precision) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

    TO_CHARS_ATTRS
    to_chars_result to_chars(char *__first, char *__last, long double __value, chars_format __fmt,
                             int __precision) GLIBCXX_NOEXCEPT {
        ours::panic(__func__);
    }

#if defined(_LIBCPP_STD_VER) && _LIBCPP_STD_VER == 23
#define LIBCPP_VERBOSE_ABORT_NOEXCEPT
#else
#define LIBCPP_VERBOSE_ABORT_NOEXCEPT noexcept
#endif

    [[noreturn]]
    // noexcept is part of the declaration in this libc++, not optional.
    void __libcpp_verbose_abort(char const *format, ...) noexcept {
        ours::panic(format);
    }
    // #ifndef _LIBCPP_STD_VER
    // }
    // #endif
    namespace placeholders {
#ifdef _LIBCPP_STD_VER
        _LIBCPP_EXPORTED_FROM_ABI const __ph<1> _1;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<2> _2;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<3> _3;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<4> _4;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<5> _5;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<6> _6;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<7> _7;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<8> _8;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<9> _9;
        _LIBCPP_EXPORTED_FROM_ABI const __ph<10> _10;
#endif
    } // namespace placeholders
} // namespace std
#ifdef _LIBCPP_STD_VER
}
#endif