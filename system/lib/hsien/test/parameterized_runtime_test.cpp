#include <hsien/runner.hpp>

namespace {

int OBSERVED_SUM = 0;

class SilentReporter final: public hsien::Reporter {
  public:
    auto report(hsien::Failure const &) -> void override {}
};

auto parameterized_test(hsien::Context &, int value) -> void {
    OBSERVED_SUM += value;
}

auto invoke_zero(hsien::Context &context) -> void {
    parameterized_test(context, 1);
}

auto invoke_one(hsien::Context &context) -> void {
    parameterized_test(context, 2);
}

auto invoke_two(hsien::Context &context) -> void {
    parameterized_test(context, 3);
}

} // namespace

auto main() -> int {
    hsien::TestRecord const records[] = {
        {"parameterized_test/0", &invoke_zero, std::source_location::current()},
        {"parameterized_test/1", &invoke_one, std::source_location::current()},
        {"parameterized_test/2", &invoke_two, std::source_location::current()},
    };
    SilentReporter reporter;
    auto const summary = hsien::TestRegistry{records, records + 3}.run(reporter);
    return summary.executed == 3 && summary.failed == 0 && OBSERVED_SUM == 6 ? 0 : 1;
}
