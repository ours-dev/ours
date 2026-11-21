#ifndef HSIEN_RUNNER_HPP
#define HSIEN_RUNNER_HPP

#include <hsien/test.hpp>

#include <span>

namespace hsien {

/// A contiguous run of test records: the image's linked section, a section a
/// framework keeps for itself, or an array a host test builds. It holds no
/// records of its own, only the bounds, so it is cheap to pass by value.
class TestRegistry final {
  public:
    constexpr TestRegistry() noexcept = default;

    /// The records in [|begin|, |end|). Either bound null is an empty
    /// registry, which is what a linker section with nothing in it may
    /// give.
    constexpr TestRegistry(TestRecord const *const begin, TestRecord const *const end) noexcept {
        if (begin != nullptr && end != nullptr) {
            m_records = std::span<TestRecord const>{begin, end};
        }
    }

    /// The records the linker placed in `.hsien.tests`. Defined only where
    /// a linker script bounds that section (the kernel), so a host build
    /// that calls it fails to link rather than running nothing.
    [[nodiscard]]
    static auto linked() noexcept -> TestRegistry;

    [[nodiscard]]
    constexpr auto records() const noexcept -> std::span<TestRecord const> {
        return m_records;
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_records.empty();
    }

    /// Runs every record once, in order, and answers how many ran and how
    /// many checks failed.
    auto run(Reporter &reporter) const noexcept -> Summary;

  private:
    std::span<TestRecord const> m_records;
};

} // namespace hsien

#endif // HSIEN_RUNNER_HPP
