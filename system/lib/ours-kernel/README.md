# ours-kernel

A hand-written codec for ours.kernel, the protocols through which the root hands out kernel resources (component-framework design S6, D19, "The protocol subset"): `DebugResource.Get` and its reply, through which a component that is routed the protocol gets the Debug resource. The ordinal is hashed from the `ours.kernel/DebugResource.Get` selector (design D20, as revised on 2026-10-04); the bodies are flat, at fixed offsets, on `//system/lib/wire`.

The include prefix and the namespace are `ours_kernel`, the library's name with its dot made an underscore; the directory is the library's name with a hyphen.

## Layout

```text
include/ours_kernel/   public headers, reached as <ours_kernel/...>
src/                   sources
test/                  host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ours-kernel`; its headers alone are `//system/lib/ours-kernel:headers`. The gtest executable is `//system/lib/ours-kernel/test:ours_kernel`, built on the host by the default build. Its ordinal is checked by `python3 system/lib/wire/test/ordinals_test.py`.
