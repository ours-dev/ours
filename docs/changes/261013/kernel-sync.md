# kernel-sync changes

## Event and Semaphore say why their operations are named as they are (2026-10-13)

**Files**
- `kernel/include/ours/sync/event.hpp` — a class comment: the model is one bit, `signal()` sets it and `unsignal()` clears it, and how that differs from a count
- `kernel/include/ours/sync/semaphore.hpp` — a paragraph: `post()` is counted, a signal is not
- `.agents/skills/ours-kernel-design/references/task.md` — "Event and Semaphore": the naming is settled

**Symptom**
The user asked why an event's operations are `signal()` and `unsignal()` rather than `signal()` and `post()`. Nothing in the code said.

**Cause**
`Event` had no class comment, so the model its names come from was left to the reader.

**Change**
Comments only. The two objects:
- **`Event`**: one bit. `signal()` sets it and wakes the waiters; `unsignal()` clears it; `wait()` returns at once while it is set.
- **`Semaphore`**: a count. `post()` adds a unit, or hands it to a waiter; `wait()` takes one, or blocks.

`post()` is the counting form of `signal()`, not its opposite; the opposite of setting a bit is clearing it, hence `unsignal()`. The difference shows when nobody waits:

| Three of these with nobody waiting | Waits that then pass at once |
|---|---|
| `Semaphore::post()` | three, each kept |
| `Event::signal()`, manual reset | every one, until `unsignal()` |
| `Event::signal()`, AutoUnsignal | one, at most one kept |

**Reference**
- Zircon `Event::Signal()` and `Unsignal()` (`zircon/kernel/include/kernel/event.h`); `Semaphore::Post()` and `Wait()` (`zircon/kernel/include/kernel/semaphore.h`).
- Linux `up()` and `down()` on a semaphore; `complete()` and `reinit_completion()` on a completion, the second playing the part of `unsignal()`.
- POSIX `sem_post()` and `sem_wait()`.

**Verification**
Comments only: built and booted with `boot_check.py`; no behaviour changed.

**Deliberately not done**
None.

**How it was found**
The user's question, 2026-10-13.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What is `post()` the opposite of, and what is `signal()` the opposite of?
2. Three posts and three signals are made while nobody waits: how many waits pass at once in each case, for both kinds of event?
3. Where in the hotplug code is an event unsignalled before it is waited on, and what would go wrong without it?
