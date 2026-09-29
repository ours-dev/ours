# launch

The user-space launcher: how a program from BootFS becomes a running process.

- `launch/plan.hpp`: `plan_mappings()`, which decides how the segments of a program are mapped (which pages come straight from the file, which are copied into VMOs of their own, and with what protection) from numbers alone. It does no I/O, so it is tested on the host.

## Layout

    include/launch/       public headers, reached as <launch/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/launch`; its headers alone are
`//system/lib/launch:headers`. Tests are executables under
`//system/lib/launch/test` and run on the host, not in the kernel.
