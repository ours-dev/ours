# numa

The machine's NUMA tables, and the node ordering derived from them

## Layout

    include/numa/   public headers, reached as <numa/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//kernel/lib/numa`; its headers alone are
`//kernel/lib/numa:headers`. Tests are executables under
`//kernel/lib/numa/test` and run on the host, not in the kernel.
