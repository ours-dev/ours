#include <hsien/runner.hpp>

namespace hsien {

extern "C" TestRecord hsien_tests_start[] asm("__hsien_tests_start");
extern "C" TestRecord hsien_tests_end[] asm("__hsien_tests_end");

auto TestRegistry::linked() noexcept -> TestRegistry {
    return TestRegistry{hsien_tests_start, hsien_tests_end};
}

} // namespace hsien
