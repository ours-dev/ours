# loop outstanding repairs

## 1. The component framework glossary still describes the Loop as holding callbacks

**Severity**
Low: a stale sentence in a document. Nothing behaves differently, but the glossary is the list of words the developer's guide sends readers to, and it contradicts the form the guide teaches.

**Symptom**
`docs/architecture/component-framework/glossary.md:49` says "**Loop**: the single-threaded event loop, a port and a set of callbacks". The Loop has no callbacks any more: a slot waits on a `loop::Waiter`, an object whose `on_signals()` the Loop calls (`system/lib/loop/include/loop/loop.hpp:74`), and `loop::Callback` is gone from the tree (`system/lib/loop/README.md:6`).

**Cause**
The glossary was written when a slot held a function and a context pointer, and the change that replaced them with a `Waiter` did not touch it.

**Change**
Write "a port and a fixed table of slots, each waiting on a `loop::Waiter`" in place of "a port and a set of callbacks". No code changes.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "callbacks" docs/architecture/component-framework/glossary.md` finds nothing about the Loop.
