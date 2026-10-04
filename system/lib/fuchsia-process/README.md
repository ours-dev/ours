# fuchsia-process

A hand-written codec for the part of fuchsia.process.Launcher that ours speaks (component-framework design S6, D13, "The protocol subset"): `Launcher.AddArgs`, `Launcher.AddHandles`, `Launcher.AddNames` and `Launcher.Launch`, with Launch's reply. The names and ordinals are Fuchsia's; the bodies are flat, at fixed offsets, on `//system/lib/wire`, and strings travel packed, each ended by a NUL, as processargs lays them out.

The include prefix and the namespace are `fuchsia_process`, as Fuchsia's own C++ bindings name the library; the directory keeps the plan's name. The Launcher's per-connection state, which judges what these messages ask for, is `launch::ProcessLauncherState` in `//system/lib/launch`.

## Layout

```text
include/fuchsia_process/   public headers, reached as <fuchsia_process/...>
src/                       sources
test/                      host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/fuchsia-process`; its headers alone are `//system/lib/fuchsia-process:headers`. The gtest executable is `//system/lib/fuchsia-process/test:fuchsia_process`, built on the host by the default build. Its ordinals are checked by `python3 system/lib/wire/test/ordinals_test.py`.
