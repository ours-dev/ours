# logz4 outstanding repairs

## 1. A log line whose body exceeds 256 bytes panics the kernel

**Severity**
Medium. Reachable today by writing an ordinary long log line, and the panic names `operator new`, which is the symptom and says nothing about the cause. Not High because no line in the tree exceeds the limit right now.

**Symptom**
The kernel dies with `[... error] operator new` at the first call of the offending line, wherever that is in boot. Nothing names logz4, the format string, or the length; both images stop at that line with no further output. Reproduced on 2026-10-28 by adding four values to `PmNode::dump()`'s per-zone line, which took its formatted body from roughly 200 bytes to roughly 310: both images panicked inside `init_pmm()`, and reverting the four values made both boot again.

**Cause**
Three pieces, each reasonable on its own.

`vlog()` formats a line's body with `ustl::fmt::vformat_to` into a bounded output iterator (`system/lib/logz4/src/log.cpp:199`):

```cpp
auto const body = ustl::fmt::vformat_to(BoundedOutput{builder.cursor(), body_end}, fmt, args);
```

`BoundedOutput` exists for a good reason, stated above its definition: a bare `char *` tells libc++ nothing about where the line buffer ends, so one long line would run off it. But it is a custom iterator, and libc++ cannot write through a custom iterator directly. It buffers into `__output_buffer`'s own `__small_buffer_[256]` (`prebuilt/clang/llvm-23/include/c++/v1/__format/buffer.h:461-462`) and flushes to the iterator in chunks; when a single formatted body does not fit the pattern it expects, it falls back to `__retarget_buffer` (`:579`), whose allocator is `allocator<_CharT>` (`:580`).

`allocator<char>::allocate()` reaches the plain `::operator new(std::size_t)`, and the kernel's definition of that is a panic (`kernel/lib/libc/cpp_dependencies.cpp:16`):

```cpp
[[gnu::weak]]
auto operator new(std::size_t) -> void * {
    ours::panic(__func__);
}
```

Nothing ever defines it strongly — the kernel's real allocating forms all take a `Gaf` (`kernel/lib/heap/include/heap/new.hpp`) — so this is not stage-dependent. A long log line panics just as readily after the heap is up as during `init_pmm()`.

So a log line is a fixed-size resource with a silent limit, and exceeding it is fatal rather than truncating. That is the opposite of what every caller will assume: the comment over `BoundedOutput` says bytes past the end are dropped, which is true of the iterator and not true of the path that reaches it.

**Change**
Make the output something libc++ writes into directly, so the retarget path cannot be taken, and let the bound be enforced by the mechanism libc++ has for it rather than by a custom iterator. `__output_buffer` already takes a `__max_output_size` (`buffer.h:362,417,446`), which is what `format_to_n` is built on, and for an unwrappable contiguous iterator it writes straight through with no small buffer at all (`:417,427`). Formatting into the line buffer as a `char *` with an explicit maximum output size should give both halves: a direct write, and truncation at the buffer's end.

Ruled out: raising `__small_buffer_`'s size, which is libc++'s and not ours to change. Ruled out as a fix on its own, though worth having anyway: asserting when the body would exceed the line buffer. It converts a confusing panic into a clear one but still loses the line, where truncating keeps the part that fits — and a log line is most wanted precisely when things are going wrong.

**Reference**
None in Linux or Zircon as such, because neither formats with libc++: Linux's `vprintk_store()` and Zircon's `fprintf`/`vsnprintf` are `vsnprintf`-shaped, which truncates to the buffer and reports the length that was needed. That is the behaviour to land on here. The nearest counterpart for the limit itself is Linux's `LOG_LINE_MAX` in `kernel/printk/printk.c`, which is 992 bytes and truncates.

**Acceptance**
A deliberately long log line — a kshell command that logs a body of about 1 KiB — truncates and the kernel keeps running, with no `operator new` panic. Both images still boot, and `PmNode::dump()` can carry its per-zone managed total and watermarks without dying (`docs/changes/261028/mm.md` records why they are logged separately today).
