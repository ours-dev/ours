# runtime

What runs before main and after it: processargs, the startup handles, and the call into main

## Layout

    include/runtime/   public headers, reached as <runtime/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/runtime`; its headers alone are
`//system/lib/runtime:headers`. Tests are executables under
`//system/lib/runtime/test` and run on the host, not in the kernel.
