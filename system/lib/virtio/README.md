# virtio

The virtio legacy device's core: the register access interface, the ring layout and the bring-up sequence, over port I/O the caller implements.

## Layout

    include/virtio/   public headers, reached as <virtio/...>
    src/                  sources
    test/                 host-side tests, built with googletest

## Building and testing

The library is `//system/lib/virtio`; its headers alone are
`//system/lib/virtio:headers`. Tests are executables under
`//system/lib/virtio/test` and run on the host, not in the kernel.
