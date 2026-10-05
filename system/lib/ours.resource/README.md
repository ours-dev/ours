# ours.resource

A hand-written codec for ours.resource, the protocols through which the root hands out kernel resources (component-framework design S6, D19, "The protocol subset"). Each protocol is named after the resource it hands out; so far there is `Debug`: `Debug.Get` and its reply, through which a component that is routed `ours.resource.Debug` gets the Debug resource. The ordinal is hashed from the `ours.resource/Debug.Get` selector (design D20, as revised on 2026-10-04); the bodies are flat, at fixed offsets, on `//system/lib/wire`.

The directory is the library's name. The include prefix and the namespace are `ours_resource`, the library's name with its dot made an underscore, since a C++ name cannot hold a dot.

## Layout

```text
include/ours_resource/   public headers, reached as <ours_resource/...>
src/                     sources
test/                    host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ours.resource`; its headers alone are `//system/lib/ours.resource:headers`. The gtest executable is `//system/lib/ours.resource/test:ours_resource`, built on the host by the default build. Its ordinal is checked by `python3 system/lib/wire/test/ordinals_test.py`.
