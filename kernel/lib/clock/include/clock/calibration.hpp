/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef CLOCK_CALIBRATION_HPP
#define CLOCK_CALIBRATION_HPP 1

#include <clock/fixed_point_ratio.hpp>

namespace ours::clock {
struct CalibrationWindow {
    u64 reference_ticks;
};

struct CalibrationConfig {
    CalibrationWindow const *windows = nullptr;
    usize window_count = 0;
    usize repeats_per_window = 2;
    u64 reference_frequency_hz = 0;
    u64 ratio_units_per_second = 1000000000;
    BinaryFixedPointRatio::Constraints ratio_constraints = {};
};

struct CalibrationTrial {
    u64 reference_ticks = 0;
    u64 device_ticks = 0;
};

struct CalibrationResult {
    [[nodiscard]]
    constexpr auto ok() const -> bool {
        return status == Status::Ok;
    }

    [[nodiscard]]
    constexpr explicit operator bool() const {
        return ok();
    }

    Status status = Status::InternalError;
    u64 frequency_hz = 0;
    BinaryFixedPointRatio ratio = {};
    CalibrationTrial trial = {};
};

class MinLatencySelector final {
  public:
    [[nodiscard]]
    constexpr auto prefer(CalibrationTrial const current, CalibrationTrial const candidate) const
        -> bool {
        return current.device_ticks == 0 || candidate.device_ticks < current.device_ticks;
    }

    [[nodiscard]]
    constexpr auto prefer_final(CalibrationTrial const current,
                                CalibrationTrial const candidate) const -> bool {
        return current.device_ticks == 0 || candidate.reference_ticks > current.reference_ticks;
    }
};

/// Shared calibration flow for architecture clocks.
///
/// Adapter requirements:
/// - prepare() -> Status
/// - begin() -> void
/// - wait_reference_ticks(u64) -> void
/// - finish() -> u64, returning elapsed ticks of the measured clock
template <typename Adapter, typename Selector = MinLatencySelector>
class Calibration final {
  public:
    [[nodiscard]]
    static auto run(Adapter &adapter, CalibrationConfig const config, Selector const selector = {})
        -> CalibrationResult {
        if (!valid_config(config)) {
            return {.status = Status::InvArgs};
        }

        auto const status = adapter.prepare();
        if (status != Status::Ok) {
            return {.status = status};
        }

        CalibrationTrial final_trial{};
        for (auto window_index = usize{0}; window_index < config.window_count; ++window_index) {
            auto const window = config.windows[window_index];
            auto const best = run_window(adapter, window, config.repeats_per_window, selector);
            if (best.device_ticks == 0) {
                continue;
            }

            if (selector.prefer_final(final_trial, best)) {
                final_trial = best;
            }
        }

        if (final_trial.device_ticks == 0) {
            return {.status = Status::InternalError};
        }

        auto const measured_frequency = rounded_mul_div(
            final_trial.device_ticks, config.reference_frequency_hz, final_trial.reference_ticks);
        if (measured_frequency == 0 || measured_frequency > ~u64{0}) {
            return {.status = Status::OutOfRange};
        }

        auto const frequency_hz = static_cast<u64>(measured_frequency);
        auto const ratio = BinaryFixedPointRatio::from_ratio(
            config.ratio_units_per_second, frequency_hz, config.ratio_constraints);
        if (!ratio) {
            return {
                .status = ratio.unwrap_err(), .frequency_hz = frequency_hz, .trial = final_trial};
        }

        return {
            .status = Status::Ok,
            .frequency_hz = frequency_hz,
            .ratio = ratio.unwrap(),
            .trial = final_trial,
        };
    }

  private:
    using Wide = unsigned __int128;

    [[nodiscard]]
    static constexpr auto valid_config(CalibrationConfig const config) -> bool {
        return config.windows != nullptr && config.window_count != 0 &&
               config.repeats_per_window != 0 && config.reference_frequency_hz != 0 &&
               config.ratio_units_per_second != 0;
    }

    [[nodiscard]]
    static auto run_window(Adapter &adapter, CalibrationWindow const window, usize const repeats,
                           Selector const selector) -> CalibrationTrial {
        if (window.reference_ticks == 0) {
            return {};
        }

        CalibrationTrial best{};
        for (auto repeat = usize{0}; repeat < repeats; ++repeat) {
            adapter.begin();
            adapter.wait_reference_ticks(window.reference_ticks);
            auto const device_ticks = adapter.finish();
            if (device_ticks == 0) {
                continue;
            }

            auto const candidate = CalibrationTrial{
                .reference_ticks = window.reference_ticks,
                .device_ticks = device_ticks,
            };
            if (selector.prefer(best, candidate)) {
                best = candidate;
            }
        }

        return best;
    }

    [[nodiscard]]
    static constexpr auto rounded_mul_div(u64 const lhs, u64 const rhs, u64 const divisor) -> Wide {
        auto const product = Wide(lhs) * rhs;
        return (product + divisor / 2) / divisor;
    }
};

} // namespace ours::clock

#endif // CLOCK_CALIBRATION_HPP
