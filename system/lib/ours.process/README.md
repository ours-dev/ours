# ours.process

A hand-written codec for ours.process.Launcher, the protocol through which a client has a process started (component-framework design S6, D13, "The protocol subset"): `Launcher.AddArgs`, `Launcher.AddHandles`, `Launcher.AddNames` and `Launcher.Launch`, with Launch's reply. Each ordinal is computed from its `ours.process/Launcher.<Method>` selector, as the header is compiled (`wire::method`) (design D20, as revised on 2026-10-04); the bodies are flat, at fixed offsets, on `//system/lib/wire`, and strings travel packed, each ended by a NUL, as processargs lays them out.

The include prefix and the namespace are `ours_process`, the library's name with its dot made an underscore, since a C++ name cannot hold a dot; the directory is the library's name. The Launcher's per-connection state, which judges what these messages ask for, is `launch::ProcessLauncherState` in `//system/lib/launch`.

## Layout

```text
include/ours_process/   public headers, reached as <ours_process/...>
src/                    sources
test/                   host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/ours.process`; its headers alone are `//system/lib/ours.process:headers`. The gtest executable is `//system/lib/ours.process/test:ours_process`, built on the host by the default build. Its ordinals are checked in `//system/lib/wire/test:wire`, against the numbers that were typed into the header before they were computed.
