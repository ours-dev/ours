# ours.resource

A hand-written codec for ours.resource, the protocols through which the root hands out kernel resources (component-framework design S6, D19, "The protocol subset"; `.agents/tasks/microkernel-interrupts/DESIGN.md`, D5). Each protocol is named after the resource it hands out and has one method, `Get`, through which a component that is routed the protocol gets that resource: `ours.resource.Debug` the System Debug resource, `ours.resource.Irq` the root of the interrupt lines (what `interrupt_map` asks for, to turn the platform's number for a pin into the number `interrupt_create` takes, and what `interrupt_create` takes for a physical line), `ours.resource.Ioport` the root of the I/O ports, `ours.resource.Mmio` the root of the physical address space, and `ours.resource.Iommu` the System IOMMU resource. The Gets share one shape and one set of functions, which take the protocol (`ours_resource::Protocol`) as an argument; each ordinal is hashed from its selector, such as `ours.resource/Irq.Get` (design D20, as revised on 2026-10-04), so one protocol's message never decodes as another's. The bodies are flat, at fixed offsets, on `//system/lib/wire`.

The directory is the library's name. The include prefix and the namespace are `ours_resource`, the library's name with its dot made an underscore, since a C++ name cannot hold a dot.

## Layout

```text
include/ours_resource/   public headers, reached as <ours_resource/...>
src/                     sources
test/                    host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ours.resource`; its headers alone are `//system/lib/ours.resource:headers`. The gtest executable is `//system/lib/ours.resource/test:ours_resource`, built on the host by the default build. Its ordinals are checked by `python3 system/lib/wire/test/ordinals_test.py`.
