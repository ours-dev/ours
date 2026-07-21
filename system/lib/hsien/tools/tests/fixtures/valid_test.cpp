#include <hsien/test.hpp>

[[=hsien::test]]
void valid_test(hsien::Context&) {}

[[=hsien::test]]
[[=hsien::value_test<>]]
[[=hsien::values<1, 2, 3>]]
void parameterized_test(hsien::Context&, int) {}

[[=hsien::fixture_test<>]]
void fixture_test(hsien::Context&) {}
