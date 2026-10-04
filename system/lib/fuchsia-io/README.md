# fuchsia-io

A hand-written codec for the part of fuchsia.io that ours speaks (component-framework design S6, "The protocol subset"): `Directory.Open`, `Node.Close`, `Cloneable.Clone`, `File.Read`, `File.GetBackingMemory` and `Directory.ReadDirents`, with their replies, the dirent packing ReadDirents uses, and fuchsia.io's flags, rights and constants. The names, bit values and ordinals are Fuchsia's; the bodies are flat, at fixed offsets, on `//system/lib/wire`.

The include prefix and the namespace are `fuchsia_io`, as Fuchsia's own C++ bindings name the library; the directory keeps the plan's name.

## Layout

```text
include/fuchsia_io/   public headers, reached as <fuchsia_io/...>
src/                  sources
test/                 host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/fuchsia-io`; its headers alone are `//system/lib/fuchsia-io:headers`. The gtest executable is `//system/lib/fuchsia-io/test:fuchsia_io`, built on the host by the default build. Its ordinals are checked by `python3 system/lib/wire/test/ordinals_test.py`.
