# svc

The provider side of a component: its outgoing directory, served on the Loop (component-framework design, "svc::Outgoing"). The counterpart of Fuchsia's `component::OutgoingDirectory` (`sdk/lib/component/outgoing/cpp`), cut to unmanaged protocols.

- `svc/outgoing.hpp`: `svc::Outgoing`, `svc::Handler`. `add_protocol(name, handler)` adds `svc/<name>`; `serve(directory_request)` serves the directory on the DirectoryRequest startup handle and waits on it. When the root forwards a client's Open of `svc/<name>`, the protocol's handler gets the client's channel, usually to add it to the same Loop.
- It is an `ioservice::Tree` served by an `ioservice::LoopServer` (`//system/lib/ioservice:loop_server`) on a `loop::Loop` (`//system/lib/loop`), so an Open through the outgoing directory is checked, and its messages are read and answered, exactly as the root's /svc is. What `svc` adds is the registry: the service node's value is an index into the handlers `add_protocol()` stored.

`//examples/echo` is its first user.

## Layout

```text
include/svc/   public headers, reached as <svc/...>
src/           sources
test/          host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/svc`; its headers alone are `//system/lib/svc:headers`. It calls the vDSO's `handle_close` and `handle_duplicate` itself; the `LoopServer` it is built on calls `channel_read`, `channel_write` and `handle_close`, and the Loop `port_wait` and `object_wait_async`. On the target the vDSO binds those calls; on the host, the fake kernel of `//system/lib/ioservice/test:fake_kernel` defines them, so the test runs the library's own source on a real Loop.

```text
prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/svc/tests
out/bringup.x86/host/obj/system/lib/svc/test/svc
```

On the target, dev's routing_checks reaches echo through it and prints `routing_checks: passed`.
