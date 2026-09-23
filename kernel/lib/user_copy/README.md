# user_copy

Moves bytes across the user/kernel boundary: typed user pointers, range checks, and the SMAP bracket around each copy.

## Layout

    include/user_copy/   public headers, reached as <user_copy/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//kernel/lib/user_copy`; its headers alone are
`//kernel/lib/user_copy:headers`. Tests are executables under
`//kernel/lib/user_copy/test` and run on the host, not in the kernel.
