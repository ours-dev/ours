# ort

The ours runtime (ort): what runs before main and after it -- processargs, the startup handles, and the call into main -- and the print helpers every program uses (`<ort/debug.hpp>`).

## Layout

    include/ort/   public headers, reached as <ort/...>
    src/           sources
    test/          host-side tests, built with googletest

## Building and testing

The library is `//system/lib/ort`; its headers alone are
`//system/lib/ort:headers`. Tests are executables under
`//system/lib/ort/test` and run on the host, not in the kernel.
