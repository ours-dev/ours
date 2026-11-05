/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// SamplerClaim.HoldsClaimUntilMemoryIsGone: a sampling session holds the
// system's one claim until its memory is gone, not only until its last handle
// is closed.
//
// Why an in-kernel test: the case is a read still copying out of a session
// whose last handle another thread has closed, and no program can hold a read
// in the middle of its copy. Here a reference to the kernel object stands in
// for that read, as sampler_read keeps one for its copy. It is not a host
// test because the claim is system-wide state inside a booted kernel.
//
//   - a session can be made, so no other one holds the claim (otherwise the
//     test is skipped);
//   - with its last handle closed and a reference kept, making another is
//     AlreadyExists: the released session's buffers are still allocated;
//   - once the reference goes, making another is Ok.
#include <ours/object/sampler_dispatcher.hpp>
#include <ours/sampler.hpp>

#include <ktest/ktest.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {

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

    KTEST(SamplerClaim, HoldsClaimUntilMemoryIsGone) {
        // The probe session is made and closed in its own scope: closing the
        // last handle releases it, and only then is the claim free for the
        // session the checks below are about.
        {
            object::KernelHandle<object::SamplerDispatcher> probe;
            auto const status = make(&probe);
            if (status == Status::AlreadyExists) {
                KTEST_SKIP("another session holds the claim");
            }
            ASSERT_OK(status, "a sampling session can be made");
        }

        // What a sampler_read copying out of the session would hold.
        ustl::Arc<object::SamplerDispatcher> reader;
        {
            object::KernelHandle<object::SamplerDispatcher> session;
            ASSERT_OK(make(&session), "a session is made");
            reader = session.dispatcher();
            // Its last handle closes here, which releases the session.
        }
        {
            object::KernelHandle<object::SamplerDispatcher> second;
            EXPECT_TRUE(
                make(&second) == Status::AlreadyExists,
                "another is AlreadyExists while a reference keeps the released one's memory");
        }
        // The read finishes: the released session and its memory go.
        reader.reset();
        {
            object::KernelHandle<object::SamplerDispatcher> third;
            EXPECT_OK(make(&third), "another is Ok once that reference goes");
        }
    }

} // namespace
} // namespace ours
