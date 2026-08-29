#ifndef HSIEN_RUNNER_HPP
#define HSIEN_RUNNER_HPP

#include <hsien/test.hpp>

namespace hsien {

auto run(TestRecord const *begin, TestRecord const *end, Reporter &reporter) noexcept -> Summary;

} // namespace hsien

#endif // HSIEN_RUNNER_HPP
