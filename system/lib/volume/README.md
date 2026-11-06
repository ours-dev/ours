# volume

The volume group's on-disk metadata and the extent mapping: how logical volumes address physical ones, linear and striped.

## Layout

    include/volume/   public headers, reached as <volume/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/volume`; its headers alone are
`//system/lib/volume:headers`. Tests are executables under
`//system/lib/volume/test` and run on the host, not in the kernel.
