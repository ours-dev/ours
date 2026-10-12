/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `samplerclaim.test`: a sampling session holds the system's one claim until
// its memory is gone, not only until its last handle is closed.
//
// Why a console command: the case is a read still copying out of a session
// whose last handle another thread has closed, and no program can hold a
// read in the middle of its copy. Here a reference to the kernel object
// stands in for that read, as sampler_read keeps one for its copy.
//
//   - a session can be made, so no other one holds the claim (otherwise the
//     test is skipped);
//   - with its last handle closed and a reference kept, making another is
//     AlreadyExists: the released session's buffers are still allocated;
//   - once the reference goes, making another is Ok.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/object/sampler_dispatcher.hpp>
#include <ours/sampler.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("samplerclaim.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("samplerclaim.test: FAIL  {}", name);
    }

    /// A session that is never started: one record per CPU, kernel target.
    constexpr auto CONFIG = SamplerConfig{
        .period = 1'000'000,
        .buffer_size = sizeof(SamplerRecord),
        .targets = SamplerTargets::Kernel,
        .reserved = 0,
    };

    /// Makes a session into |out|, whose handle closes as |out| goes.
    auto make(object::KernelHandle<object::SamplerDispatcher> *const out) -> Status {
        auto rights = Rights::None;
        return object::SamplerDispatcher::create(CONFIG, out, &rights);
    }

    auto samplerclaim_test_command(int, char **) -> int {
        {
            object::KernelHandle<object::SamplerDispatcher> probe;
            auto const status = make(&probe);
            if (status == Status::AlreadyExists) {
                log::info("samplerclaim.test: skipped -- another session holds the claim");
                return 0;
            }
            if (status != Status::Ok) {
                log::error("samplerclaim.test: could not make a session: {}", to_string(status));
                return 1;
            }
        }

        Tally tally;
        // What a sampler_read copying out of the session would hold.
        ustl::Arc<object::SamplerDispatcher> reader;
        {
            object::KernelHandle<object::SamplerDispatcher> session;
            auto const made = make(&session) == Status::Ok;
            verdict(tally, made, "a session is made");
            if (made) {
                reader = session.dispatcher();
            }
            // Its last handle closes here, which releases the session.
        }
        {
            object::KernelHandle<object::SamplerDispatcher> second;
            verdict(tally, make(&second) == Status::AlreadyExists,
                    "another is AlreadyExists while a reference keeps the released one's memory");
        }
        // The read finishes: the released session and its memory go.
        reader.reset();
        {
            object::KernelHandle<object::SamplerDispatcher> third;
            verdict(tally, make(&third) == Status::Ok, "another is Ok once that reference goes");
        }

        if (tally.failed != 0) {
            log::error("samplerclaim.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("samplerclaim.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(SamplerClaimTest, "samplerclaim.test", ours::samplerclaim_test_command,
             "check that a released sampling session holds its claim until its memory is gone");
