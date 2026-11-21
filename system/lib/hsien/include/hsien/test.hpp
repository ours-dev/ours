#ifndef HSIEN_TEST_HPP
#define HSIEN_TEST_HPP

#include <source_location>

#include <cstddef>

namespace hsien {

struct test_t {};

inline constexpr test_t test{};

template <auto... Values>
struct values_t {};

template <auto... Values>
inline constexpr values_t<Values...> values{};

struct Failure {
    char const *test_name;
    std::source_location location;
    char const *message;
};

/// Where a failed check goes. A Context hands every failure to the reporter
/// the run was given; what a failure becomes -- a console line in the
/// kernel, a recorded entry in a host test -- is the derived class's.
///
/// Nothing deletes a reporter through this base: each lives in static
/// storage or on the frame that runs the tests. So the destructor is
/// protected and not virtual, which keeps a delete through the base from
/// compiling instead of paying for a virtual destructor nothing calls.
class Reporter {
  public:
    virtual auto report(Failure const &failure) -> void = 0;

    Reporter(Reporter const &) = delete;
    auto operator=(Reporter const &) -> Reporter & = delete;
    Reporter(Reporter &&) = delete;
    auto operator=(Reporter &&) -> Reporter & = delete;

  protected:
    Reporter() = default;
    ~Reporter() = default;
};

/// One test's view of the run: the name it reports under, the reporter its
/// failures go to, and how many of its checks failed so far.
class Context final {
  public:
    constexpr Context(char const *test_name, Reporter &reporter) noexcept
        : m_test_name(test_name), m_reporter(&reporter) {}

    auto check(bool condition, std::source_location location = std::source_location::current(),
               char const *message = "") noexcept -> bool;

    [[nodiscard]]
    constexpr auto failure_count() const noexcept -> std::size_t {
        return m_failure_count;
    }

  private:
    char const *m_test_name;
    Reporter *m_reporter;
    std::size_t m_failure_count{};
};

/// One test as the image carries it. An aggregate on purpose: the records
/// are constant data the test macros place in a linker section with
/// designated initializers, and nothing changes one after the build.
struct TestRecord {
    char const *name;
    void (*invoke)(Context &);
    std::source_location location;

    /// Runs the test once against a fresh Context and answers how many of
    /// its checks failed.
    auto run(Reporter &reporter) const noexcept -> std::size_t;
};

struct Summary {
    std::size_t executed{};
    std::size_t failed{};
};

} // namespace hsien

#endif // HSIEN_TEST_HPP
