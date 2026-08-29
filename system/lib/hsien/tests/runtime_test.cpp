#include <source_location>

#include <hsien/runner.hpp>

namespace {

bool second_ran = false;
std::size_t report_count = 0;

auto report(hsien::Failure const &) -> void {
    ++report_count;
}

auto first_test(hsien::Context &context) -> void {
    context.check(false, std::source_location::current(), "expected failure");
    context.check(true, std::source_location::current(), "must continue");
}

auto second_test(hsien::Context &) -> void {
    second_ran = true;
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
                   expect(report_count == 1) && expect(second_ran)
               ? 0
               : 1;
}
