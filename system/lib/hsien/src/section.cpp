#include <hsien/section.hpp>

namespace hsien {

extern "C" TestRecord hsien_tests_start[] asm("__hsien_tests_start");
extern "C" TestRecord hsien_tests_end[] asm("__hsien_tests_end");

auto section_begin() noexcept -> TestRecord const * {
    return hsien_tests_start;
}

auto section_end() noexcept -> TestRecord const * {
    return hsien_tests_end;
}

auto run_section(Reporter &reporter) noexcept -> Summary {
    return run(section_begin(), section_end(), reporter);
}

} // namespace hsien
