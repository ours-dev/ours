# ours.io

A hand-written codec for ours.io, the protocol files and directories are served over (component-framework design S6, "The protocol subset"): `Directory.Open`, `Node.Close`, `Cloneable.Clone`, `File.Read`, `File.GetBackingMemory` and `Directory.ReadDirents`, with their replies, the dirent packing ReadDirents uses, and the protocol's flags, rights and constants. Each ordinal is computed from its `ours.io/<Protocol>.<Method>` selector, as the header is compiled (`wire::method`) (design D20, as revised on 2026-10-04); the bodies are flat, at fixed offsets, on `//system/lib/wire`.

The include prefix and the namespace are `ours_io`, the library's name with its dot made an underscore, since a C++ name cannot hold a dot; the directory is the library's name.

## Layout

```text
include/ours_io/   public headers, reached as <ours_io/...>
src/               sources
test/              host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ours.io`; its headers alone are `//system/lib/ours.io:headers`. The gtest executable is `//system/lib/ours.io/test:ours_io`, built on the host by the default build. Its ordinals are checked in `//system/lib/wire/test:wire`, against the numbers that were typed into the header before they were computed.
