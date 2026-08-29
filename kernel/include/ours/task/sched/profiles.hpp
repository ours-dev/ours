/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_PROFILES_HPP
#define OURS_TASK_SCHED_PROFILES_HPP 1

#include <ours/compiler.hpp>
#include <ours/task/sched/types.hpp>

namespace ours::task::sched {
enum class Discipline {
    Normal,
    Deadline,
    Count,
};

struct BaseProfile {
    constexpr explicit BaseProfile(i32 priority, bool inheritible = true)
        : weight(), discipline() {}

    ~BaseProfile() = default;

    SchedWeight weight;
    Discipline discipline{Discipline::Normal};
};

/// The profile which be really used by scheduler in scheduling work.
struct EffectiveProfile {
    EffectiveProfile() = default;
    ~EffectiveProfile() = default;

    constexpr explicit EffectiveProfile(BaseProfile const &bp)
        : weight(bp.weight), discipline(bp.discipline) {}

    auto notify_base_profile_overrided() -> void {}

    SchedWeight weight;
    SchedDuration initial_timeslice_ns;
    Discipline discipline{Discipline::Normal};
};

/// The profile which tracks all of inherited values from upstream.
struct InheritedProfile {
    using Self = InheritedProfile;

    auto snapshot() -> Self {
        return Self(*this);
    }

    SchedWeight inherited_weight;
};

struct Profile {
    FORCE_INLINE constexpr Profile(BaseProfile const &profile)
        : base(profile), effective(profile), inherited() {}

    ~Profile() = default;

    auto recompute_effective_profile() -> void;

    BaseProfile base;
    EffectiveProfile effective;
    InheritedProfile inherited;
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_PROFILES_HPP