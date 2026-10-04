# wire

The framing every hand-written protocol codec here shares (component-framework design D20, `docs/design/261004/component-framework.md`): the 16-byte header shaped like Zircon's `fidl_message_header_t` with a direction byte in place of the first at-rest flag, `HANDLE_PRESENT` placeholders for handle fields, the 24-byte status form that every failed reply and every epitaph takes, and `wire::Method`, which declares a method's ordinal beside the selector it is hashed from.

It works on bytes and handle values alone and makes no system call, so it builds in every toolchain, and its tests run on the host. The codecs built on it are `//system/lib/fuchsia-io` and, later, the Launcher and DebugResource codecs.

## Layout

```text
include/wire/   public headers, reached as <wire/...>
src/            sources
test/           host-side tests, built with googletest, and ordinals_test.py
```

## Building and testing

The library is `//system/lib/wire`; its headers alone are `//system/lib/wire:headers`. The gtest executable is `//system/lib/wire/test:wire`, built on the host by the default build.

`test/ordinals_test.py` recomputes every ordinal that a codec header declares with `wire::Method` from its selector, with Python's hashlib, since the tree has no SHA-256 in C++:

```text
python3 system/lib/wire/test/ordinals_test.py
```
