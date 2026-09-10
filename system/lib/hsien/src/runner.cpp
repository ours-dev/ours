#include <hsien/runner.hpp>

namespace hsien {

auto Context::check(bool condition, std::source_location location, char const *message) noexcept
    -> bool {
    if (condition) {
        return true;
    }

    ++m_failure_count;
    if (m_reporter != nullptr && m_reporter->report != nullptr) {
        m_reporter->report(Failure{
            .test_name = m_test_name,
            .location = location,
            .message = message,
        });
    }
    return false;
}

auto run(TestRecord const *begin, TestRecord const *end, Reporter &reporter) noexcept -> Summary {
    Summary summary{};
    if (begin == nullptr || end == nullptr) {
        return summary;
    }

    for (auto const *record = begin; record != end; ++record) {
        ++summary.executed;
        Context context(record->name, reporter);
        record->invoke(context);
        summary.failed += context.failure_count();
    }
    return summary;
}

} // namespace hsien
