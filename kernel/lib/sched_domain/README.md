# sched_domain

The scheduling domains of a CPU: the nested sets of CPUs it looks at for work, nearest first, each with its own parameters. The scheduler walks them instead of asking the memory manager which node a CPU is on every time it looks.

## Layout

    include/sched_domain/   the one public header, reached as <sched_domain/...>; the library is header only
    test/                   host-side tests, built with googletest

## Building and testing

The library is `//kernel/lib/sched_domain`; its headers alone are
`//kernel/lib/sched_domain:headers`. Tests are executables under
`//kernel/lib/sched_domain/test` and run on the host, not in the kernel.
