# kernel-log changes

## A long log or panic body no longer reaches operator new (2026-11-13)

**Files**

- `system/lib/ustl/include/ustl/fmt/format_to.hpp` — new: `ustl::fmt::vformat_to_n(char *out, n, fmt, args)`, the bounded, allocator-free write the alias layer was missing.
- `system/lib/logz4/src/log.cpp` — the line body formats through the new primitive; the `BoundedOutput` iterator class is gone, its truncation contract carried by the bound and `result.size`.
- `system/public/ours/assert.hpp` — `detail::render()` writes through the same shape; `PanicSink::Iterator` is gone, the sink's cursor advances out of the format's answer.
- `kernel/top/panic.cpp` — `vpanic(fmt, args)` writes through the same shape.

**Symptom**

`ktest all shuffle` panicked the kernel: `OURS KERNEL PANIC / operator new`, no summary, the machine rebooted. The plain `ktest all` was green. Latent beyond that run: any kernel log line whose formatted body exceeds 256 bytes panics the same way, and any panic message whose rendered text exceeds 256 bytes recurses into `operator new` from inside the panic already in progress.

**Cause**

`ustl::fmt::vformat_to` is a using-alias for libc++'s `std::vformat_to`. For an output iterator that is neither a back inserter nor a raw pointer — logz4's `BoundedOutput`, the panic machinery's `PanicSink::Iterator` — libc++ buffers the whole formatted text through `__format::__allocating_buffer`, which keeps a 256-byte stack array and then silently **grows it with `operator new`**. This kernel's unflagged `operator new` is a deliberate panic stub (`kernel/lib/libc/cpp_dependencies.cpp`), so the write the caller bounded, expecting truncation, reached an allocator instead.

The log side tripped first because logz4's bounded design was real but sat one layer below the trap: the `BoundedOutput` iterator dropped bytes past its end, yet libc++ had already grown its own buffer before any byte reached the iterator. The registry that crosses the threshold is the ktest one: the `SHUFFLE` line's `order=` text is one registry index per test, and at 79 linked tests the body passes 256 bytes. The 2026-11-07 shuffle record verified the composition at 64 tests, where the same line fit — the bug was born with the alias over the allocating path and only became reachable when a line grew past libc++'s buffer.

The panic side is the severity carrier and had never fired: a panic message over 256 bytes would allocate, the allocation would panic, and the panic would try to render again — recursion until the stack dies, on the one path that must not fail.

**Change**

Every bounded sink now writes through the one shape the standard machinery runs without an allocator: a raw `char *` with an explicit bound. libc++ dispatches on the iterator — back inserters get chunked flushing, `char *` (and only `char *`) gets `__direct_iterator_buffer`, everything else gets the allocating buffer — so the bound moves from a custom iterator's `operator=` into the call:

```cpp
// before: the bound lives in the iterator's operator=, but libc++ never sees it;
// the text accumulates in __allocating_buffer and grows with operator new
class BoundedOutput { ... };   // operator= drops past m_last
auto const body = ustl::fmt::vformat_to(BoundedOutput{builder.cursor(), body_end}, fmt, args);
if (body.dropped()) { ... }

// after: the bound is the call's; the write lands directly, nothing allocates
auto const body_room = static_cast<std::ptrdiff_t>(builder.remaining() - reserve);
auto const body = ustl::fmt::vformat_to_n(builder.cursor(), body_room, fmt, args);
if (body.size > body_room) { ... }   // result.size counts what the format demanded
```

`ustl::fmt::vformat_to_n` is the layer's own name for libc++'s `std::__vformat_to_n<std::format_context>` — the "v" (pre-built `format_args`) form the public `format_to_n` lacks, with a `char *` first parameter so the non-allocating dispatch is the only one the type system allows. `system/public/ours/assert.hpp` cannot depend on ustl (its TUs include panic headers alone), so its `render()` spells the same internal directly.

**Reference**

libc++'s own buffer selection (`prebuilt/clang/llvm-23/include/c++/v1/__format/buffer.h`, `__buffer_selector` and the comment on `__container_inserter_buffer`: "An alternative would be to use an allocating buffer") — the direct-pointer path and the 256-byte chunked paths exist exactly so a bounded write need not allocate; the bug was reaching the third selector branch. GoogleTest's shuffle contract ("tests must be independent of order", `googletest/include/gtest/gtest.h`'s `--gtest_shuffle`) is what made the long line reachable, and its shape — flag-based shuffling of an otherwise fixed walk — is what the repair's verification runs through.

**Verification**

- `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest all shuffle repeat=2 shard=0/1'`: before the fix, `OURS KERNEL PANIC / operator new` at the SHUFFLE line (also reproduced on the unmodified tip with the refactor stashed and rebuilt, so the bug predates the ktest work); after the fix, `ktest: SHUFFLE seed=0x243f6a8885a308d3 order=69,1,65,...` — the full 79-index line, no truncation at the ring's 512-byte lines — then `SUMMARY ran=158 passed=154 failed=0 skipped=4`.
- `boot_check.py --out out/bringup.x86`: dbg PASS 46/5/0, rel PASS 44/8/0 (7/7 required), host-tests 183/0.
- `ktest all`: `SUMMARY ran=79 passed=77 failed=0 skipped=2` — the two known skips unchanged.
- clang-format clean on all four files; clang-tidy reports nothing on the changed lines.

**Deliberately not done**

- Teaching `__allocating_buffer` a kernel allocator or relaxing the unflagged `operator new` stub: the stub is the design — kernel code does not allocate silently — and the allocating path is libc++ internal behavior this tree cannot reshape.
- A `std::format_to_n`-shaped fix without pre-built args: logz4's `vlog_dispatch` and `ours::panic` take assembled `format_args` (the physload export boundary and the assert.hpp layering require it), and the public `format_to_n` is variadic-only; the internal's "v" form is what they need.
- Removing the `Iterator`-shaped API from `PanicSink` beyond what the change orphans: the struct keeps its two pointers and its contract; nothing else referenced the iterator.

**How it was found**

The ktest refactor's verification gate ran the composition the 2026-11-07 record had verified; it panicked. The trace to the root cause went through the panic reason (`operator new`, `panic(__func__)` in the stub), a temporary frame-chain print in the stub (reverted) naming `__allocating_buffer::__prepare_write` → `__handle_replacement_field` → `__vformat_to` → `logz4::vlog` → `ktest::log_shuffle`, and the static side: `ustl/fmt/format_to.hpp` is `using std::vformat_to`, and only `char *` output escapes libc++'s allocating buffer.

**The user's prediction**

None (defect repair; the decision to fix it inside this change was taken with the coordinator, option A of three).

**Three questions you should be able to answer**

- Why did the same `SHUFFLE` line pass at 64 tests and panic at 79?
- Why does a class-shaped output iterator reach the allocator while a raw `char *` does not?
- Why is the panic path's expression of this bug worse than the log's, and why had it never fired?
