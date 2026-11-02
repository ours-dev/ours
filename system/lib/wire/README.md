# wire

The framing every hand-written protocol codec here shares (component-framework design D20, `docs/design/261004/component-framework.md`): the 16-byte header shaped like Zircon's `fidl_message_header_t` with a direction byte in place of the first at-rest flag, `HANDLE_PRESENT` placeholders for handle fields, the 24-byte status form that every failed reply and every epitaph takes, and `wire::Method`, which a codec declares with `wire::method(selector, kind)`: the method's ordinal is computed from its selector as the header is compiled (a SHA-256 the compiler runs, `wire/sha256.hpp`), so no number is typed, and `wire::all_distinct` lets the header `static_assert` that its methods have different ordinals.

It works on bytes and handle values alone and makes no system call, so it builds in every toolchain, and its tests run on the host. The codecs built on it are `//system/lib/ours.io`, `//system/lib/ours.process` and `//system/lib/ours.resource`.

## Layout

```text
include/wire/   public headers, reached as <wire/...>
src/            sources
test/           host-side tests, built with googletest
```

## Building and testing

The library is `//system/lib/wire`; its headers alone are `//system/lib/wire:headers`. The gtest executable is `//system/lib/wire/test:wire`, built on the host by the default build. It checks the SHA-256 against the examples of FIPS 180-4 and against hashes at the padding boundaries, and checks the ordinals of the codec headers against the numbers that were typed into them by hand before they were computed.
