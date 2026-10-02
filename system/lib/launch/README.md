# launch

The user-space launcher: how a program from BootFS becomes a running process.

- `launch/plan.hpp`: `Plan::build()`, which decides how the segments of a program are mapped from numbers alone: which pages come straight from the file, which from a copy-on-write snapshot of it, which from a zero-fill VMO with the file bytes of one partial page copied in, and with what protection each is mapped. It does no I/O, so it is tested on the host.

## Layout

    include/launch/       public headers, reached as <launch/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/launch`; its headers alone are
`//system/lib/launch:headers`. Tests are executables under
`//system/lib/launch/test` and run on the host, not in the kernel.
