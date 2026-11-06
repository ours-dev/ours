/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// BootOptions.*: the steal options of a command line are read by index, in the
// order of the knobs (a mismatch is a compile-time error in domains.cpp). Only
// the well-formed cases are run: a malformed one is logged as an error, which
// `ktest` takes for a failed run, and was read by hand.
//
// StealPolicy.*: the knobs that steer a look for threads to take
// (task::sched::StealPolicy) hold values their ranges accept after boot,
// whatever the image's command line said of them, and set() takes a value in
// range and refuses one out of it, leaving the knob as it was.
//
// The values themselves are not compared with the defaults: an image may have
// set a knob on its command line (kernel.sched.steal.<name>), and a test of
// the knob must not fail on that. What is checked is the contract, and each
// knob is put back as the test found it.
#include <ours/boot/options.hpp>
#include <ours/task/sched/domains.hpp>

#include <string_view>

#include <ktest/ktest.hpp>
#include <obi/command_line_view.hpp>

namespace ours {
namespace {
    using sched_domain::Knob;
    using task::sched::StealPolicy;

    /// Puts a knob back as it was when the test is done with it, however the
    /// test ends.
    class Restore {
      public:
        explicit Restore(Knob const knob) : m_knob(knob), m_value(StealPolicy::get(knob)) {}
        Restore(Restore const &) = delete;
        auto operator=(Restore const &) -> Restore & = delete;
        ~Restore() {
            // The value saved was one the knob held, so the knob takes it.
            auto _ = StealPolicy::set(m_knob, m_value);
        }

      private:
        Knob m_knob;
        usize m_value;
    };

    /// What |text| says of the |index|-th steal option, or 0xFFFFFFFF for none.
    auto steal_option_of(std::string_view const text, usize const index) -> u32 {
        auto const view = obi::CommandLineView::from_text(text);
        if (view.is_err()) {
            return 0xFFFFFFFF;
        }
        boot::BootOptions options;
        options.apply(view.unwrap());
        auto const given = options.steal_option(index);
        if (given.is_none()) {
            return 0xFFFFFFFF;
        }
        return given.value();
    }

    KTEST(BootOptions, TheStealOptionsAreReadByIndex) {
        EXPECT_EQ(steal_option_of("", 0), 0xFFFFFFFFu, "nothing said, nothing given");
        EXPECT_EQ(steal_option_of("kernel.shell=true", 3), 0xFFFFFFFFu, "another option");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.enabled=false", 0), 0u, "the word false");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.enabled=0", 0), 0u, "0");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.enabled=true", 0), 1u, "the word true");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.enabled", 0), 1u, "the bare key is true");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.min-runnable-same-node=5", 1), 5u, "1");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.min-runnable-other-node=6", 2), 6u, "2");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.max-batch-same-node=7", 3), 7u, "3");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.max-batch-other-node=8", 4), 8u, "4");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.max-batch-same-node=7", 4), 0xFFFFFFFFu,
                  "each option is its own index's");
        EXPECT_EQ(steal_option_of("a=b kernel.sched.steal.max-batch-same-node=2 c", 3), 2u,
                  "among other tokens");
        EXPECT_EQ(steal_option_of("kernel.sched.steal.max-batch-same-node=2", 5), 0xFFFFFFFFu,
                  "an index past the options");
    }

    KTEST(StealPolicy, EveryKnobIsInItsRangeAfterBoot) {
        for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
            auto const knob = static_cast<Knob>(i);
            EXPECT_TRUE(sched_domain::knob_accepts(knob, StealPolicy::get(knob)),
                        "a knob holds a value it accepts");
        }
    }

    KTEST(StealPolicy, ASettingIsReadBackAndARefusedOneChangesNothing) {
        for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
            auto const knob = static_cast<Knob>(i);
            auto const info = sched_domain::knob_info(knob);
            Restore const restore{knob};

            EXPECT_TRUE(StealPolicy::set(knob, info.highest), "the highest value is accepted");
            EXPECT_EQ(StealPolicy::get(knob), info.highest, "and read back");
            EXPECT_TRUE(StealPolicy::set(knob, info.lowest), "the lowest value is accepted");
            EXPECT_EQ(StealPolicy::get(knob), info.lowest, "and read back");

            EXPECT_FALSE(StealPolicy::set(knob, info.highest + 1), "a value above the range");
            EXPECT_EQ(StealPolicy::get(knob), info.lowest, "is refused and changes nothing");
            if (info.lowest > 0) {
                EXPECT_FALSE(StealPolicy::set(knob, info.lowest - 1), "a value below the range");
                EXPECT_EQ(StealPolicy::get(knob), info.lowest, "is refused and changes nothing");
            }
        }
    }

    KTEST(StealPolicy, TheAccessorsReadTheKnobsOfTheirKind) {
        Restore const same_min{Knob::MinRunnableSameNode};
        Restore const other_min{Knob::MinRunnableOtherNode};
        Restore const same_batch{Knob::MaxBatchSameNode};
        Restore const other_batch{Knob::MaxBatchOtherNode};
        Restore const enabled{Knob::Enabled};

        EXPECT_TRUE(StealPolicy::set(Knob::MinRunnableSameNode, 5), "set");
        EXPECT_TRUE(StealPolicy::set(Knob::MinRunnableOtherNode, 6), "set");
        EXPECT_TRUE(StealPolicy::set(Knob::MaxBatchSameNode, 2), "set");
        EXPECT_TRUE(StealPolicy::set(Knob::MaxBatchOtherNode, 3), "set");
        EXPECT_TRUE(StealPolicy::set(Knob::Enabled, 0), "set");
        EXPECT_EQ(StealPolicy::min_runnable(sched_domain::TierKind::SameNode), 5u, "same node");
        EXPECT_EQ(StealPolicy::min_runnable(sched_domain::TierKind::OtherNode), 6u, "other node");
        EXPECT_EQ(StealPolicy::max_batch(sched_domain::TierKind::SameNode), 2u, "same node");
        EXPECT_EQ(StealPolicy::max_batch(sched_domain::TierKind::OtherNode), 3u, "other node");
        EXPECT_FALSE(StealPolicy::enabled(), "off");
        EXPECT_TRUE(StealPolicy::set(Knob::Enabled, 1), "set");
        EXPECT_TRUE(StealPolicy::enabled(), "on");
    }
} // namespace
} // namespace ours
