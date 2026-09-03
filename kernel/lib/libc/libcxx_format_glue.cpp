/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The <format> machinery that ours/phys/print.hpp reaches — and any other
/// header-only libc++ formatting code — calls out-of-line basic_string<char>
/// members (`push_back`, `__grow_by_and_replace`, ...). libc++ ships those
/// in its compiled library, which the freestanding -nostdlib++ images never
/// link, so the definitions have to come from an explicit instantiation.
///
/// This glue carries ONLY that instantiation: the exception/abort hooks live
/// in kernel/lib/ccrt (which phys images already link), and duplicating them
/// here would break the phys link with duplicate symbols.

#include <ours/panic.hpp>

#include <charconv>
#include <string>

namespace std {
// Emits every out-of-line member of basic_string<char> that the <format>
// formatter family references.
template class basic_string<char>;

// The explicit class instantiation above does not reach member function
// templates; the string-assignment paths the formatter uses call these, so
// they need their own instantiation definitions. [temp.explicit] exempts
// explicit instantiations from access checking, which is what makes naming
// this private member legal.
template auto basic_string<char>::__assign_no_alias<false>(char const *, std::size_t)
    -> basic_string &;
template auto basic_string<char>::__assign_no_alias<true>(char const *, std::size_t)
    -> basic_string &;

// The floating-point formatters need libc++'s library-level float to_chars.
// The kernel's rule applies to the boot images too: no floating point, so
// these stubs exist only to be linked past and panic if ever reached.
//
// The stubs are free functions, so they have to sit inside libc++'s ABI
// inline namespace for their mangled names to match the references — the
// same trick libcxx.cpp's own to_chars definitions use.
inline namespace _LIBCPP_ABI_NAMESPACE {
    template <typename F>
    auto to_chars_float_stub(char *, char *, F) -> to_chars_result {
        ours::panic("phys: floating-point formatting is not supported");
    }

    to_chars_result to_chars(char *first, char *last, float value) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, double value) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, long double value) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, float value, chars_format) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, double value, chars_format) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, long double value, chars_format) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, float value, chars_format, int) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, double value, chars_format, int) {
        return to_chars_float_stub(first, last, value);
    }
    to_chars_result to_chars(char *first, char *last, long double value, chars_format, int) {
        return to_chars_float_stub(first, last, value);
    }
} // namespace _LIBCPP_ABI_NAMESPACE
} // namespace std
