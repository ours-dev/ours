# thread_sampler

The kernel half of the sampling profiler: the session a Sampler object stands for, its per-CPU timers and what it records into. Its namespace is `sampler`.

## Layout

    include/thread_sampler/   public headers, reached as <thread_sampler/...>
    src/                      sources

## Building and testing

The library is `//kernel/lib/thread_sampler`; its headers alone are `//kernel/lib/thread_sampler:headers`. It is built only for the kernel and has no host tests: it is made of the kernel's timers, IPIs and locks. What a sample contains, and how a stack is unwound, is `//kernel/lib/prof`'s and `//kernel/lib/unwind`'s, which are tested on the host. The session itself is exercised by the dbg image's `sampler` and `profile` shell checks.
