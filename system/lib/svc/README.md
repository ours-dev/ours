# svc

The provider side of a component: its outgoing directory, served on the Loop (component-framework design, "svc::Outgoing"). The counterpart of Fuchsia's `component::OutgoingDirectory` (`sdk/lib/component/outgoing/cpp`), cut to unmanaged protocols.

- `svc/outgoing.hpp`: `svc::Outgoing`, `svc::Handler`. `add_protocol(name, handler)` adds `svc/<name>`; `serve(directory_request)` serves the directory on the DirectoryRequest startup handle and waits on it. When the root forwards a client's Open of `svc/<name>`, the protocol's handler gets the client's channel, usually to add it to the same Loop.
- It is a `ioservice::Tree` and `ioservice::Server` (`//system/lib/ioservice`) on a `loop::Loop` (`//system/lib/loop`), so an Open through the outgoing directory is checked exactly as one through the root's /svc is.

`//examples/echo` is its first user.

## Layout

```text
include/svc/   public headers, reached as <svc/...>
src/           sources
test/          host-side tests, built with googletest, and the fake kernel
```

## Building and testing

The library is `//system/lib/svc`; its headers alone are `//system/lib/svc:headers`. It calls the vDSO's `channel_read`, `channel_write`, `handle_close` and `handle_duplicate`, and its Loop `port_wait` and `object_wait_async`. On the target the vDSO binds those calls; on the host, `test/fake_kernel.cpp` defines them, so the test runs the library's own source on a real Loop.

```text
prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/svc/tests
out/bringup.x86/host/obj/system/lib/svc/test/svc
```

On the target, dev's routing_checks reaches echo through it and prints `routing_checks: passed`.
