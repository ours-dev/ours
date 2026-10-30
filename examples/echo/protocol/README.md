# ours_examples_echo

The ours.examples.echo protocol, encoded and decoded by hand on `//system/lib/wire`: the protocol `//examples/echo` provides, and the one dev's routing checks reach it by, through an offer from a sibling (component-framework plan, step 17).

- `Echo.EchoString` is Fuchsia's, from its routing example (`examples/components/routing/fidl/echo.fidl`), with a string that is always present.
- `Echo.Stop` is ours's own: the provider answers, then exits, so that a client can see the next route to it answer NotFound (design D23). Fuchsia stops a component through its Lifecycle channel instead, which ours does not have.

The ordinals are fidlc's, computed from the selectors as the header is compiled (`wire::method`).

## Layout

```text
include/ours_examples_echo/   public headers, reached as <ours_examples_echo/...>
src/                          sources
test/                         host-side tests, built with googletest
```

## Building and testing

The library is `//examples/echo/protocol`; its headers alone are `//examples/echo/protocol:headers`. The gtest executable is `//examples/echo/protocol/test:ours_examples_echo`, built on the host by the default build.
