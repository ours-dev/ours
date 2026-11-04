# ddk

The runtime helpers every userspace driver restates by hand, as objects
whose destructors own the teardown orders: the resource ladder, startup
handles, MMIO windows, the DMA chain, and the interrupt idiom.

## What a driver writes

```cpp
ddk::Resources resources;
auto const mmio = resources.take(ddk::Kind::Mmio);   // ... Ioport, Iommu, Irq
auto const vma = ddk::startup_handle(ours::HandleType::VmaRoot, "VmaRoot");
auto window = ddk::MappedMmio::map(vma.unwrap(), mmio.unwrap(), base, size);
auto bti = ddk::Bti::create(iommu.unwrap());
auto region = ddk::DmaRegion::allocate(vma.unwrap(), bti->handle(), bytes);
auto interrupt = ddk::irq::take(irq.unwrap(), domain, source, options);
```

Any early return below a take unwinds through the destructors: the ladder
closes what it still holds, the window unmaps, the region unmaps then
unpins — the order that keeps the kernel from quarantining what was pinned.
`resources.drop(handle)` releases a root early, for the roots a driver is
done with (a root is a connection from a finite pool).

## The alignment rule

Helpers consume handles their caller holds; only `Resources` acquires, and
only through a getter that a host test can replace. This is what keeps the
kit valid when the driver manager's `Claim` lands: `Claim` returns handles,
and every helper here keeps taking handles.

## Layout

```text
include/ddk/            public headers, reached as <ddk/...>
src/                     sources
test/                    host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ddk`; its headers alone are
`//system/lib/ddk:headers`. The host suite runs under
`//system/lib/ddk:tests`, on the fake kernel of
`//system/lib/testing/fake_kernel`. The reference drivers are
`//system/driver/virtio-blk` (the full ladder) and
`//system/service/acpi-route` (startup handles alone).
