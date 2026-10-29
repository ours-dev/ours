# loop

One port and a fixed table of wait slots, through which a single-threaded server waits on many handles at once: the root's every wait (`docs/design/261004/component-framework.md`, "Concurrency: one thread and one Loop"), and later `ioservice::Outgoing`'s. The counterpart of Fuchsia's async-loop (`sdk/lib/async-loop/loop.c`), cut down to waits.

- `loop/loop.hpp`: `Loop`, `Slot`, `Key`, `Action`, `Waiter` and `Callback`. `add()` takes the lowest free slot and arms a one-shot wait whose key is `generation << 32 | index`; `run()` takes packets, calls each slot's callback once, and re-arms the slot or closes it as the callback answers.
- A slot waits on a `Waiter`, an abstract class whose `on_signals()` `run()` calls (`add(handle, signals, waiter)`), or, in the older form, on a function and a `void *` context (`Callback`). The two share the table and the slot. `Callback` goes in a later step of `.agents/tasks/loop-waiter/PLAN.md`, when the last registration has moved to a `Waiter`; until then a `Waiter` registration is made over it, and a call to one makes the indirect call it always did, to a function of the Loop's, and that function makes the virtual one.
- `run()` is the function P3 in `.agents/tasks/component-framework/PLAN.md` reserves for the user to write by hand. Its comment in `loop.hpp` is the whole spec. For the MVP of 2026-10-04 the AI wrote it on the user's instruction; the hand-writing exercise stays open.

## Layout

```text
include/loop/   public headers, reached as <loop/...>
src/            sources
test/           host-side tests, built with googletest, and the fake kernel
```

## Building and testing

The library is `//system/lib/loop`; its headers alone are `//system/lib/loop:headers`. It calls the vDSO's `port_wait`, `object_wait_async` and `handle_close` and nothing else. On the target the vDSO binds those calls; on the host, `test/fake_kernel.cpp` defines the same three symbols, so the test runs the library's own source.

```text
prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests
out/bringup.x86/host/obj/system/lib/loop/test/loop
```

The shell's `check_loop()` (`system/app/shell/shell_tests.cpp`) runs the Loop on real channels and a real port, on both images, and prints `shell: loop checks passed`.
