# fsio

The client side of a process's namespace (component-framework design, `docs/design/261004/component-framework.md`), ours's counterpart of the part of fdio a component uses:

- `<fsio/path.hpp>`: `clean_path()`, fdio's `CleanPath` (Plan 9's lexical rules for "." and "..").
- `<fsio/namespace.hpp>`: `Namespace`, fdio's `fdio_ns_t` as a flat table, with fdio's rules for what may be bound where (absolute paths, no binding below or above another), and the lookup that finds the directory a path lies under.
- `<fsio/fsio.hpp>`: the calls through it, `install()` from the NsDir startup handles (run by the runtime before main, through `ort_extensions_init()`, which this library defines), `open()`, `connect()`, `read()`, `read_dirents()`, `get_backing_memory()` and `close()`, speaking ours.io (`//system/lib/ours.io`); and `export_namespace()` and `spawn()`, fdio's `fdio_ns_export_root()` and `fdio_spawn()`, which hand the namespace on through the ours.process.Launcher in `/svc` (`//system/lib/ours.process`).

The first two make no system call and build in every toolchain, the host one included, where their tests run. The calls exist only in the user toolchain and are checked on the target, by the dev image's `routing_checks` component (`//system/service/init/test/routing_checks`), which also spawns itself through `spawn()`.

## Layout

```text
include/fsio/   public headers, reached as <fsio/...>
src/            sources: path.cpp and namespace.cpp everywhere, fsio.cpp in user programs
test/           host-side tests, built with googletest
```

## Building and testing

The calls are `//system/lib/fsio`, defined in the user toolchain only; path cleaning and the namespace table alone are `//system/lib/fsio:namespace`, and the headers `//system/lib/fsio:headers`. The gtest executable is `//system/lib/fsio/test:fsio`, built on the host by the default build, which also compiles the calls in the user toolchain.
