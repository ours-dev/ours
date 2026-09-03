/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_PROFILES_HPP
#define OURS_TASK_SCHED_PROFILES_HPP 1

#include <ours/compiler.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/sched/types.hpp>

namespace ours::task::sched {
enum class Discipline {
    Normal,
    Deadline,
    Count,
};

struct BaseProfile {
    /// The priority is what the caller has and the weight is what the
    /// scheduler computes with, so the translation belongs here, at the one
    /// point a profile is made from a priority. It used to be dropped on the
    /// floor -- `weight()` value-initialized to 0 whatever the priority --
    /// and priority_to_weight() sat in common.hpp with no caller. A weight
    /// of 0 is not a low weight, it is no weight: every thread contributed
    /// nothing to the run queue's weight sum, and avg_vruntime() divided by
    /// that sum.
    ///
    /// Zircon's BaseProfile(int priority) does the same conversion in the
    /// same place, via SchedulerState::ConvertPriorityToWeight().
    constexpr explicit BaseProfile(i32 priority, bool inheritible = true)
        : weight(priority_to_weight(static_cast<usize>(priority))), discipline() {}

    ~BaseProfile() = default;

    Weight weight;
    Discipline discipline{Discipline::Normal};
};

/// The profile which be really used by scheduler in scheduling work.
struct EffectiveProfile {
    EffectiveProfile() = default;
    ~EffectiveProfile() = default;

    constexpr explicit EffectiveProfile(BaseProfile const &bp)
        : weight(bp.weight), discipline(bp.discipline) {}

    auto notify_base_profile_overrided() -> void {}

    Weight weight;
    SchedDuration initial_timeslice_ns;
    Discipline discipline{Discipline::Normal};
};

/// The profile which tracks all of inherited values from upstream.
struct InheritedProfile {
    using Self = InheritedProfile;

    auto snapshot() -> Self {
        return Self(*this);
    }

    Weight inherited_weight;
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