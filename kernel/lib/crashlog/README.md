# crashlog

The panic crash record: a CRC-checked record in a byte region that outlives a warm reset.

## Layout

    include/crashlog/   public headers, reached as <crashlog/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//kernel/lib/crashlog`; its headers alone are
`//kernel/lib/crashlog:headers`. Tests are executables under
`//kernel/lib/crashlog/test` and run on the host, not in the kernel.
