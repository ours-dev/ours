/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_STACK_SAMPLE_REQUEST_HPP
#define SAMPLER_STACK_SAMPLE_REQUEST_HPP 1

namespace sampler {
/// A request to sample a thread's user stack: where the sampling timer found
/// the thread in user space, and which run of which session asked. The timer
/// makes one and hands it to the thread; the thread keeps it, opaque, and
/// carries it out on its way back to user space, where reading user memory
/// may fault.
///
/// What the values mean is the sampler's, and so is the rule they serve: a
/// thread may act on a request late -- preempted or suspended first -- and
/// the run number lets the sampler drop one that a session started since
/// never asked for.
///
/// No lock: only the thread itself touches its request, written in its own
/// timer interrupt and read on the way out of that same interrupt, before it
/// returns to user space, where a new one could be made.
///
/// A header of its own, with nothing beyond the prelude, because the thread
/// holds it by value: the task layer needs the complete type without
/// depending on the rest of the sampler.
class StackSampleRequest final {
  public:
    constexpr StackSampleRequest() = default;

    /// The interrupted user pc and frame pointer, and the run that asked.
    constexpr StackSampleRequest(u64 const pc, u64 const fp, u64 const generation)
        : m_pc(pc), m_fp(fp), m_generation(generation) {}

    /// Walks the user stack from where the timer found it and records it
    /// into the session that asked, if that run is still the one running.
    /// The calling thread's own request, on its way back to user space, with
    /// interrupts on.
    auto record() const -> void;

  private:
    u64 m_pc = 0;
    u64 m_fp = 0;
    u64 m_generation = 0;
};
} // namespace sampler

#endif // SAMPLER_STACK_SAMPLE_REQUEST_HPP
