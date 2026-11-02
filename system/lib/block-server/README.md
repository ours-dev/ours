# block_server

The block-driver library: a BlockServer any block device driver subclasses, which parks requests in slots and replies on hardware completion

## Layout

    include/block_server/   public headers, reached as <block_server/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/block-server`; its headers alone are
`//system/lib/block-server:headers`. Tests are executables under
`//system/lib/block-server/test` and run on the host, not in the kernel.
