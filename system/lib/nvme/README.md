# nvme

The NVMe controller's device core: the register file, the queue pairs and the bring-up sequence, over an MMIO mapping the caller owns.

## Layout

    include/nvme/   public headers, reached as <nvme/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/nvme`; its headers alone are
`//system/lib/nvme:headers`. Tests are executables under
`//system/lib/nvme/test` and run on the host, not in the kernel.
