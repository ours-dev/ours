# fuchsia-kernel

A hand-written codec for the part of fuchsia.kernel that ours speaks (component-framework design S6, D19, "The protocol subset"): `DebugResource.Get` and its reply, through which a component that is routed the protocol gets the Debug resource. The name and ordinal are Fuchsia's; the bodies are flat, at fixed offsets, on `//system/lib/wire`.

The include prefix and the namespace are `fuchsia_kernel`, as Fuchsia's own C++ bindings name the library; the directory keeps the plan's name.

## Layout

```text
include/fuchsia_kernel/   public headers, reached as <fuchsia_kernel/...>
src/                      sources
test/                     host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/fuchsia-kernel`; its headers alone are `//system/lib/fuchsia-kernel:headers`. The gtest executable is `//system/lib/fuchsia-kernel/test:fuchsia_kernel`, built on the host by the default build. Its ordinal is checked by `python3 system/lib/wire/test/ordinals_test.py`.
