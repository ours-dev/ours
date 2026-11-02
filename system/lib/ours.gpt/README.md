# ours_gpt

GPT, the partition table a disk carries, read over a block device

## Layout

    include/ours_gpt/   public headers, reached as <ours_gpt/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/ours.gpt`; its headers alone are
`//system/lib/ours.gpt:headers`. Tests are executables under
`//system/lib/ours.gpt/test` and run on the host, not in the kernel.
