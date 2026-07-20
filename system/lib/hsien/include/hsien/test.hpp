#ifndef HSIEN_TEST_HPP
#define HSIEN_TEST_HPP

#include <cstddef>
#include <source_location>

namespace hsien {

struct test_t {};

inline constexpr test_t test {};

struct Failure {
    char const* test_name;
    std::source_location location;
    char const* message;
};

using ReportFn = void (*)(Failure const&);

struct Reporter {
    ReportFn report {};
};

class Context final {
public:
    constexpr Context(char const* test_name, Reporter& reporter) noexcept
        : m_test_name(test_name), m_reporter(&reporter) {}

    auto check(bool condition,
               std::source_location location = std::source_location::current(),
               char const* message = "") noexcept -> bool;

    constexpr auto failure_count() const noexcept -> std::size_t {
        return m_failure_count;
    }

private:
    char const* m_test_name;
    Reporter* m_reporter;
    std::size_t m_failure_count {};
};

struct TestRecord {
    char const* name;
    void (*invoke)(Context&);
    std::source_location location;
};

struct Summary {
    std::size_t executed {};
    std::size_t failed {};
};

} // namespace hsien

#endif // HSIEN_TEST_HPP
