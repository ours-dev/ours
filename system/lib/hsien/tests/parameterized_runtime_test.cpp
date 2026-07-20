#include <hsien/runner.hpp>

namespace {

int observed_sum = 0;

auto report(hsien::Failure const&) -> void {}

auto parameterized_test(hsien::Context&, int value) -> void {
    observed_sum += value;
}

auto invoke_zero(hsien::Context& context) -> void {
    parameterized_test(context, 1);
}

auto invoke_one(hsien::Context& context) -> void {
    parameterized_test(context, 2);
}

auto invoke_two(hsien::Context& context) -> void {
    parameterized_test(context, 3);
}

} // namespace

auto main() -> int {
    hsien::TestRecord const records[] = {
        {"parameterized_test/0", &invoke_zero, std::source_location::current()},
        {"parameterized_test/1", &invoke_one, std::source_location::current()},
        {"parameterized_test/2", &invoke_two, std::source_location::current()},
    };
    hsien::Reporter reporter {&report};
    auto const summary = hsien::run(records, records + 3, reporter);
    return summary.executed == 3 && summary.failed == 0 && observed_sum == 6 ? 0 : 1;
}
