/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// One declaration, many records: the value-parameterized and typed tests.
// `KTEST_VALUE_PARAM` declares a suite's named constexpr value table,
// `KTEST_P`/`KTEST_F_P` declare one record per value over it, and
// `KTEST_TYPE_LIST` with `KTEST_T` does the same over a list of types. Each
// instantiation is its own `hsien::TestRecord` named `Suite.Name/0`,
// `Suite.Name/1`, ... — GoogleTest's `/N` suffix convention — so the runner
// reports and selects them exactly like plain tests and none of this needs
// runner changes.
//
// Everything here is free of kernel headers, so the host suite runs it against
// the real macro expansions; only the section the record blocks land in is the
// kernel's.
#ifndef KTEST_INSTANTIATE_HPP
#define KTEST_INSTANTIATE_HPP 1

#include <ktest/fixture.hpp>
#include <ktest/run_state.hpp>

#include <source_location>
#include <utility>

#include <gktl/types.hpp>
#include <hsien/test.hpp>

namespace ktest {

namespace internal {

    /// The value half of a value-parameterized suite: the table value this
    /// instantiation's object was built with. The class `KTEST_VALUE_PARAM`
    /// declares derives from this — beside the fixture for `KTEST_F_P`, beside
    /// the test's own `Fixture` base for `KTEST_P` — so a body reads the
    /// current value with `GetParam()` through the same `this` that reaches
    /// the fixture's members. Deriving from the suite's base, not being one
    /// itself, is what keeps a fixture's single `Fixture` subobject when the
    /// two compose.
    template <typename Param>
    class ParamSource {
      public:
        using ParamType = Param;

        explicit ParamSource(ParamType const &value) : m_value(value) {}

        /// The table value this instantiation was built with.
        [[nodiscard]]
        auto GetParam() const -> ParamType const & {
            return m_value;
        }

      private:
        ParamType m_value;
    };

    /// One instantiation's record name: the test's `Suite.Name` with the `/N`
    /// suffix that tells its records apart, built at compile time because a
    /// record's name is part of the image and no runner allocates it. The
    /// capacity is the prefix's length — `sizeof` of the declaring array, one
    /// past the terminator — plus 21: one slash, the twenty digits a usize
    /// suffix can reach, and the terminator.
    template <usize Capacity>
    struct InstantiationName {
        char value[Capacity]{};

        consteval InstantiationName(char const *prefix, usize const index) {
            auto at = usize{0};
            for (; prefix[at] != '\0'; ++at) {
                value[at] = prefix[at];
            }
            value[at] = '/';
            at += 1;
            char digits[20];
            auto count = usize{0};
            auto rest = index;
            do {
                digits[count] = static_cast<char>('0' + rest % 10);
                rest /= 10;
                count += 1;
            } while (rest != 0);
            for (auto i = usize{0}; i < count; ++i) {
                value[at + i] = digits[count - 1 - i];
            }
            value[at + count] = '\0';
        }
    };

    /// The records one `KTEST_P`/`KTEST_F_P`/`KTEST_T` declares: a constexpr
    /// array placed in `.hsien.tests` as one object. The runner reads the
    /// whole section as a flat array of TestRecord, which stays true because
    /// every element is sizeof(TestRecord) apart and the block's alignment is
    /// the record's, so no padding ever lands between the section's objects.
    /// An empty table or type list is an authoring bug the compiler reports.
    template <usize Count>
    struct RecordBlock {
        static_assert(Count != 0,
                      "a KTEST_VALUE_PARAM table or a KTEST_TYPE_LIST declares no elements");
        ::hsien::TestRecord records[Count]{};
    };

    /// The types one `KTEST_T` instantiates over, as `KTEST_TYPE_LIST`
    /// declares them. The count is all the records side needs; the types
    /// travel as the list's own template parameters, paired with their
    /// positions when the block is built.
    template <typename... Types>
    struct TypeList {
        static constexpr usize COUNT = sizeof...(Types);
    };

} // namespace internal

} // namespace ktest

/// Declares the named constexpr value table of one value-parameterized suite:
/// |table| is the array as written, and the class `Suite_ParamBase` carries
/// its type, length and elements to the suite's `KTEST_P`/`KTEST_F_P` tests.
/// One table per suite: a second `KTEST_VALUE_PARAM` for the same suite
/// redefines the class, which the compiler reports.
#define KTEST_VALUE_PARAM(suite, type, table, ...)                                                 \
    inline constexpr type table[] = {__VA_ARGS__};                                                 \
    class KTEST_JOIN(suite, _ParamBase) : public ::ktest::internal::ParamSource<type> {            \
      public:                                                                                      \
        using ParamType = type;                                                                    \
        static constexpr usize COUNT = sizeof(table) / sizeof(table[0]);                           \
        static constexpr type const *VALUES = table;                                               \
        explicit KTEST_JOIN(suite, _ParamBase)(type const &value)                                  \
            : ::ktest::internal::ParamSource<type>(value) {}                                       \
    }

/// The machinery one `KTEST_P`/`KTEST_F_P` expands into beside its test class:
/// the object storage, one invoke per instantiation, and the constexpr records
/// block. |test_class| is the generated class, |param_base| the suite's
/// `KTEST_VALUE_PARAM` base, and the two tokens are stringified into the
/// records' `Suite.Name` prefix. Sharing the expansion is what makes the
/// fixture combination one line rather than a second machinery.
#define KTEST_P_INSTANTIATE(test_class, param_base, suite_token, name_token)                       \
    template <usize Index>                                                                         \
    auto test_class::invoke(::hsien::Context &context) -> void {                                   \
        ::ktest::internal::run_fixture<test_class>(                                                \
            context, test_class::storage, [](test_class &test) { test.ktest_body(); },             \
            param_base::VALUES[Index]);                                                            \
    }                                                                                              \
    struct test_class::Records {                                                                   \
        static constexpr char PREFIX[] = #suite_token "." #name_token;                             \
        template <usize Index>                                                                     \
        struct Holder {                                                                            \
            static constexpr ::ktest::internal::InstantiationName<sizeof(PREFIX) + 21> NAME{       \
                PREFIX, Index};                                                                    \
            static constexpr ::hsien::TestRecord RECORD{                                           \
                .name = NAME.value,                                                                \
                .invoke = &test_class::invoke<Index>,                                              \
                .location = ::std::source_location::current(),                                     \
            };                                                                                     \
        };                                                                                         \
        template <usize... Indices>                                                                \
        static consteval auto BLOCK(::std::integer_sequence<usize, Indices...>)                    \
            -> ::ktest::internal::RecordBlock<sizeof...(Indices)> {                                \
            return {{Holder<Indices>::RECORD...}};                                                 \
        }                                                                                          \
    };                                                                                             \
    alignas(test_class) unsigned char test_class::storage[sizeof(test_class)];                     \
    KTEST_RECORD_SECTION(".hsien.tests")                                                           \
    FORCE_USED inline ::ktest::internal::RecordBlock<param_base::COUNT> const                      \
        test_class::records =                                                                      \
            test_class::Records::BLOCK(::std::make_integer_sequence<usize, param_base::COUNT>{});

/// Defines one instantiation family of a value-parameterized suite: one record
/// per entry of the suite's `KTEST_VALUE_PARAM` table, named `Suite.Name/0`
/// upward, each running the same body with the next table value. The body is
/// a member of `Suite_Name_Test`, which derives from `ktest::Fixture` and the
/// suite's param base, so it reads the current value with `GetParam()` and the
/// whole lifecycle of a `KTEST_F` — a fresh object per record, SetUp before
/// the body, TearDown after it — comes free.
#define KTEST_P(suite, name)                                                                       \
    class suite##_##name##_Test final: public ::ktest::Fixture<suite##_##name##_Test>,             \
                                       public suite##_ParamBase {                                  \
      public:                                                                                      \
        using suite##_ParamBase::suite##_ParamBase;                                                \
        void ktest_body();                                                                         \
        template <usize Index>                                                                     \
        static auto invoke(::hsien::Context &context) -> void;                                     \
        struct Records;                                                                            \
        static unsigned char storage[];                                                            \
        static ::ktest::internal::RecordBlock<suite##_ParamBase::COUNT> const records;             \
    };                                                                                             \
    KTEST_P_INSTANTIATE(suite##_##name##_Test, suite##_ParamBase, suite, name)                     \
    void suite##_##name##_Test::ktest_body()

/// The same over a fixture: the test's object derives from |fixture_class| and
/// the suite's param base, so one instantiation sees the fixture's SetUp-built
/// members and its own table value. The fixture must be default constructible,
/// exactly as for `KTEST_F`.
#define KTEST_F_P(fixture_class, name)                                                             \
    class fixture_class##_##name##_Test final: public fixture_class,                               \
                                               public fixture_class##_ParamBase {                  \
      public:                                                                                      \
        using fixture_class##_ParamBase::fixture_class##_ParamBase;                                \
        void ktest_body();                                                                         \
        template <usize Index>                                                                     \
        static auto invoke(::hsien::Context &context) -> void;                                     \
        struct Records;                                                                            \
        static unsigned char storage[];                                                            \
        static ::ktest::internal::RecordBlock<fixture_class##_ParamBase::COUNT> const records;     \
    };                                                                                             \
    KTEST_P_INSTANTIATE(fixture_class##_##name##_Test, fixture_class##_ParamBase, fixture_class,   \
                        name)                                                                      \
    void fixture_class##_##name##_Test::ktest_body()

/// Removes one level of parentheses: `KTEST_TYPE_LIST` receives its list
/// parenthesized because a macro parameter cannot be a bare comma list beside
/// other parameters.
#define KTEST_UNPAREN(...) __VA_ARGS__

/// Declares the type list of one typed suite: `KTEST_TYPE_LIST(Suite, (A, B))`
/// is what the suite's `KTEST_T` tests instantiate over. One list per suite, as
/// for `KTEST_VALUE_PARAM`.
#define KTEST_TYPE_LIST(suite, types)                                                              \
    using suite##_TypeList = ::ktest::internal::TypeList<KTEST_UNPAREN types>;

/// Defines one instantiation family of a typed suite: one record per type of
/// the suite's `KTEST_TYPE_LIST`, named `Suite.Name/0` upward. The body is a
/// static function template whose parameter is named `T`, so the body reads
/// the record's own type as `T`; each record's invoke runs the body with the
/// type its position in the list pairs it with.
#define KTEST_T(suite_token, name_token)                                                           \
    class suite_token##_##name_token##_Test {                                                      \
      public:                                                                                      \
        template <typename T>                                                                      \
        static void body();                                                                        \
        template <typename T>                                                                      \
        static auto invoke(::hsien::Context &context) -> void {                                    \
            ::ktest::internal::BindScope const scope(context);                                     \
            body<T>();                                                                             \
        }                                                                                          \
        struct Records;                                                                            \
        static ::ktest::internal::RecordBlock<suite_token##_TypeList::COUNT> const records;        \
    };                                                                                             \
    struct suite_token##_##name_token##_Test::Records {                                            \
        static constexpr char PREFIX[] = #suite_token "." #name_token;                             \
        template <usize Index, typename T>                                                         \
        struct Holder {                                                                            \
            static constexpr ::ktest::internal::InstantiationName<sizeof(PREFIX) + 21> NAME{       \
                PREFIX, Index};                                                                    \
            static constexpr ::hsien::TestRecord RECORD{                                           \
                .name = NAME.value,                                                                \
                .invoke = &suite_token##_##name_token##_Test::invoke<T>,                           \
                .location = ::std::source_location::current(),                                     \
            };                                                                                     \
        };                                                                                         \
        template <typename... Types, usize... Indices>                                             \
        static consteval auto BLOCK(::ktest::internal::TypeList<Types...>,                         \
                                    ::std::integer_sequence<usize, Indices...>)                    \
            -> ::ktest::internal::RecordBlock<sizeof...(Indices)> {                                \
            return {{Holder<Indices, Types>::RECORD...}};                                          \
        }                                                                                          \
    };                                                                                             \
    KTEST_RECORD_SECTION(".hsien.tests")                                                           \
    FORCE_USED inline ::ktest::internal::RecordBlock<suite_token##_TypeList::COUNT> const          \
        suite_token##_##name_token##_Test::records =                                               \
            suite_token##_##name_token##_Test::Records::BLOCK(                                     \
                suite_token##_TypeList{},                                                          \
                ::std::make_integer_sequence<usize, suite_token##_TypeList::COUNT>{});             \
    template <typename T>                                                                          \
    void suite_token##_##name_token##_Test::body()

#endif // #ifndef KTEST_INSTANTIATE_HPP
