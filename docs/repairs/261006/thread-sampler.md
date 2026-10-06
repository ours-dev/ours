# thread-sampler outstanding repairs

## 1. A thread keeps the sampler's user-stack request as three loose fields that only the sampler understands

**Severity**
Low: encapsulation. Nothing behaves wrongly today.

**Symptom**
`kernel/include/ours/task/thread.hpp` declares three fields on `Thread`: `m_sample_pc`, `m_sample_fp` and `m_sample_session`.

- `Thread::Current::request_stack_sample()` writes them, in `kernel/kernel/task/thread.cpp`, around line 802. Its caller is the sampling timer, in `kernel/lib/thread_sampler/src/thread_sampler.cpp`, around line 418.
- `thread.cpp`, around line 789, reads all three back and passes them to `sampler::Session::sample_user_stack()`.

**Cause**
The three values are one fact, and it belongs to the sampler. What they mean, that they are written and read together, and that the session tag lets a mark acted on late be dropped rather than recorded into a session started since: all of that is sampler knowledge.

`Thread` holds them as unrelated members, so the invariant has no owner. The style skill's rule is "the object owns its invariant, and it is the only one who does". As things stand, adding a field or changing the stale-mark rule means editing the task layer.

**Change**
1. The sampler library provides a value type, for example `sampler::StackSampleRequest`. It keeps pc, fp and session private and offers two verbs:
   - `set(pc, fp, session)`, called from the timer interrupt;
   - `record()`, which hands the request to `Session::sample_user_stack()` on the way back to user space.
2. `Thread` holds one `sampler::StackSampleRequest m_stack_sample`. `request_stack_sample()` forwards to `set()`, and `thread.cpp`'s three reads become one `record()`.
3. The type lives in a header-only GN target of its own, for example `//kernel/lib/thread_sampler:request`, with no dependency beyond the prelude. `thread.hpp` must hold it by value, so it needs the complete type. The rest of `thread_sampler` depends on the task layer, so depending on all of it from `thread.hpp` would make a GN cycle. Both the task layer and `thread_sampler` depend on the small target instead.
4. The fields stay without a lock, as today: only the thread itself touches them, in its own timer interrupt and on the way out of it. The type's comment keeps that argument.

Ruled out:

- **A pointer to an incomplete type.** The request would then need storage somewhere else, and an allocation in the timer interrupt.
- **Including all of `thread_sampler` from `thread.hpp`.** It is the GN cycle above.

Owner: `thread_sampler` is the profiling session's (ours-refactor-reasonix-06) area. Coordinate with it before starting, because it may have this code in flight.

**Reference**
None, this project's own design rule ("the object owns its invariant", modern-cpp-development).

**Acceptance**
- `Thread` has no `m_sample_*` fields; `git grep -n 'm_sample_' kernel/include/ours/task/thread.hpp` finds nothing.
- `gn gen` reports no dependency cycle.
- The build is clean, `boot_check.py` passes, and the profile example (`examples/profile`) still records user-stack samples, as its log lines show.
