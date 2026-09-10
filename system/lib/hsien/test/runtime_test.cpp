#include <hsien/runner.hpp>

#include <source_location>

namespace {

bool SECOND_RAN = false;
std::size_t REPORT_COUNT = 0;

auto report(hsien::Failure const &) -> void {
    ++REPORT_COUNT;
}

auto first_test(hsien::Context &context) -> void {
    context.check(false, std::source_location::current(), "expected failure");
    context.check(true, std::source_location::current(), "must continue");
}

auto second_test(hsien::Context &) -> void {
    SECOND_RAN = true;
}

auto expect(bool condition) -> bool {
    return condition;
}

} // namespace

auto main() -> int {
    hsien::TestRecord const records[] = {
        {"first_test", &first_test, std::source_location::current()},
        {"second_test", &second_test, std::source_location::current()},
    };
    hsien::Reporter reporter{&report};
    auto const summary = hsien::run(records, records + 2, reporter);

    return expect(summary.executed == 2) && expect(summary.failed == 1) &&
                   expect(REPORT_COUNT == 1) && expect(SECOND_RAN)
               ? 0
               : 1;
}
