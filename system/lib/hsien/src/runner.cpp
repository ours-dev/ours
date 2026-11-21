#include <hsien/runner.hpp>

namespace hsien {

auto Context::check(bool condition, std::source_location location, char const *message) noexcept
    -> bool {
    if (condition) {
        return true;
    }

    ++m_failure_count;
    m_reporter->report(Failure{
        .test_name = m_test_name,
        .location = location,
        .message = message,
    });
    return false;
}

auto TestRecord::run(Reporter &reporter) const noexcept -> std::size_t {
    Context context(name, reporter);
    invoke(context);
    return context.failure_count();
}

auto TestRegistry::run(Reporter &reporter) const noexcept -> Summary {
    Summary summary{};
    for (auto const &record: m_records) {
        ++summary.executed;
        summary.failed += record.run(reporter);
    }
    return summary;
}

} // namespace hsien
