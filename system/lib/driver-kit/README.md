# driver_kit

The runtime helpers every userspace driver restates by hand: the resource ladder, startup handles, MMIO mapping, the DMA chain, and the interrupt idiom

## Layout

    include/driver_kit/   public headers, reached as <driver_kit/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/driver-kit`; its headers alone are
`//system/lib/driver-kit:headers`. Tests are executables under
`//system/lib/driver-kit/test` and run on the host, not in the kernel.
