#ifndef HSIEN_SECTION_HPP
#define HSIEN_SECTION_HPP

#include <hsien/runner.hpp>

namespace hsien {

auto section_begin() noexcept -> TestRecord const *;
auto section_end() noexcept -> TestRecord const *;
auto run_section(Reporter &reporter) noexcept -> Summary;

} // namespace hsien

#endif // HSIEN_SECTION_HPP
