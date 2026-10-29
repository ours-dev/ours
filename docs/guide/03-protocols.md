# Protocols: the messages a server and its clients exchange

## What you will be able to do

After this chapter you will be able to:

- say what a protocol is, and which files make one in this tree;
- choose a protocol's name, its methods, and the number each method goes by;
- lay a message out byte by byte, and read one in a hex dump;
- decide, for any message a server receives, whether to answer it, to close the connection, or to say why with an epitaph;
- put a handle in a message, and keep every message within its size limits;
- add a new protocol library to the tree, with its build target and its test.

This chapter is about the messages. The program that answers them is chapter 2 (in preparation); a program that serves a directory tree instead is [chapter 4](04-tree-servers.md). The kernel's words (handle, channel, epitaph, manifest) are defined in [chapter 1](01-concepts.md); each is also explained in a line where it first matters here.

## What a protocol is

A *protocol* is the agreement between a server and its clients about what they write on one channel: which kinds of request exist, what bytes each message holds, and what comes back. A [channel](01-concepts.md#handles-and-channels) is a two-ended pipe; one end writes a *message*, the other end reads it. A message is some bytes and, optionally, some handles.

Words used throughout this chapter:

- *method*: one kind of request, such as `EchoString`.
- *two-way method*: the server sends exactly one *reply* for each request (`wire::MethodKind::TwoWay`).
- *one-way method*: the server sends no reply (`wire::MethodKind::OneWay`).
- *codec*: the code that turns a C++ struct into the bytes of a message (an *encoder*) and the bytes of a message back into a struct (a *decoder*).

In this tree a protocol is a library of its own, and it holds the protocol's name, its methods and a codec for each message. The server and every client link the same library, so they cannot disagree about a layout. The codecs are written by hand today (`docs/architecture/component-framework/libraries.md`, "Why it is split this way"), on top of a framing library called `wire` that holds what every protocol shares: the header, the form of a failed reply, and the epitaph (`system/lib/wire/README.md:3`).

A codec never touches a channel. It works on buffers of bytes that you give it, and the program hands what an encoder returned to `channel_write` (`system/lib/wire/include/wire/wire.hpp:41-43`). Because it makes no system call it builds on the host, and its tests run there (`system/lib/wire/README.md:5`).

A decoder is also the server's first line of defence. A server reads what programs it does not trust send it (`system/lib/wire/test/wire_test.cpp:5-7`), so every decoder checks everything it reads. The decoders return the tree's result type, `ustl::Result<T, ours::Status>`: `is_ok()` and `is_err()` ask which it is, `unwrap()` takes the value and `unwrap_err()` the status.

## The running example

This chapter follows one real protocol, `ours.examples.echo`, which lives in `examples/echo/protocol`. It is the protocol of the example server `examples/echo/echo_server.cpp`. It has two methods: `EchoString` sends a string and gets the same string back, and `Stop` asks the server to answer and then exit (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:17-24`).

```text
examples/echo/protocol/
  BUILD.gn                                    the build targets
  README.md                                   what the library is
  include/ours_examples_echo/
    ours_examples_echo.hpp                    names, methods, structs, declarations
  src/ours_examples_echo.cpp                  the encoders and decoders
  test/BUILD.gn                               the host test target
  test/ours_examples_echo_test.cpp            the host tests
```

The header declares the protocol's name and its one tunable limit.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:46-49`:

```cpp
inline constexpr auto ECHO_NAME = std::string_view{"ours.examples.echo.Echo"};

/// The longest string EchoString carries.
inline constexpr auto MAX_STRING_LENGTH = usize{64};
```

It declares each method as a `wire::Method`, which pairs a *selector* (the method's full name) with its *ordinal* (the number the method goes by on the wire) and its kind. Both words are explained in "Methods and their numbers" below.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:55-60`:

```cpp
/// Answers with the string it was given (Echo.EchoString).
inline constexpr auto ECHO_STRING = wire::Method{
    .selector = "ours.examples.echo/Echo.EchoString",
    .ordinal = 0x6e80'6bd7'7638'6789,
    .kind = wire::MethodKind::TwoWay,
};
```

It declares a struct for what a message carries, in the form the program uses it.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:69-74`:

```cpp
struct EchoStringRequest {
    u32 txid;
    /// At most MAX_STRING_LENGTH bytes. Decoded, it views the message's
    /// bytes.
    std::string_view value;
};
```

And it declares the functions that turn that struct into a message and back. The encoder writes into a buffer the caller supplies (`bytes`) and returns the `wire::Message` to write; the decoder reads a `wire::Message` and returns the struct, or an error status.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:93-98`:

```cpp
[[nodiscard]]
auto encode_echo_string(std::span<u8> bytes, EchoStringRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_echo_string(wire::Message message) -> ustl::Result<EchoStringRequest, ours::Status>;
```

The header's comment (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:26-37`) also writes every message's layout as a table of fields and offsets, and states the rules about the string. Do the same for your protocol: the layout is part of the interface, and a reader should not have to read the decoder to learn it.

## Names and the protocol type

Three names are involved, and they are easy to confuse.

| Name | Echo's | Used for |
|---|---|---|
| Library name | `ours.examples.echo` | The first part of the protocol name and of every selector. |
| Protocol name | `ours.examples.echo.Echo` (`ECHO_NAME`) | Manifests, `/svc/<name>` in a client, `svc/<name>` in a provider's outgoing directory. |
| C++ name | `ours_examples_echo` | The include prefix, the namespace, the test target. |

The protocol name is the library name, a dot, and the protocol's own name (`system/lib/ours.process/include/ours_process/ours_process.hpp:59-60`, `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:49-50`). The C++ name is the library name with its dots made underscores, because a C++ name cannot hold a dot (`system/lib/ours.resource/BUILD.gn:10-13`). The protocol name is what a component finds the protocol under in its `/svc` and what a provider serves it under in the `svc/` of its outgoing directory; a route may rename it on the way (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:42-47`). Both places are explained in [`/svc` and the outgoing directory](01-concepts.md#svc-and-the-outgoing-directory). The provider registers the name with its outgoing directory in one call (`examples/echo/echo_server.cpp:268`).

A component's manifest says which protocols it provides, hands up and needs ([components and manifests](01-concepts.md#components-and-manifests)). Echo's manifest declares the protocol and exposes it to its parent.

From `examples/echo/BUILD.gn:36-48`:

```gn
  manifest = {
    capabilities = [
      {
        protocol = "ours.examples.echo.Echo"
      },
    ]
    expose = [
      {
        protocol = "ours.examples.echo.Echo"
        from = "self"
      },
    ]
  }
```

The *protocol type* is a string that says which protocol an entry speaks. It is written `type` in a manifest and, when left out, equals the entry's own name (`system/component_manifest.gni:109-111`). It exists because an `offer` may rename a capability with `as`, after which the name no longer says what the protocol is. The dev image's root offers echo's protocol to `routing_checks` under another name (`kernel/products/image/dev/BUILD.gn:183-191`), so `routing_checks` states in its `use` which protocol it means.

From `system/service/init/test/routing_checks/BUILD.gn:85-88`:

```gn
      {
        protocol = "ours.test.Echo"
        type = "ours.examples.echo.Echo"
      },
```

A route must end at a capability of the type its use states, and this is checked twice. The build checks every route in a manifest's whole tree (`system/component_manifest.gni:62-65`). The root checks again on every Open: it logs a line containing `reaches another protocol` and closes the component's channel with the epitaph `BadType` (`system/service/init/realm.cpp:188-197` and `:740-743`; `system/lib/component/include/component/route.hpp:303-306`).

The type is only a name. It says nothing about a protocol's layout: two protocols that share a name and differ in layout are not told apart (`docs/design/261029/protocol-identity.md:30`). So choose a name that is yours, and keep it for one layout.

## Methods and their numbers

Every request names the method it calls by a 64-bit number, the *ordinal*, in bytes 8 to 15 of its header. Nobody picks an ordinal by hand: it is computed from the method's *selector*, a string that names the method.

By the tree's convention a selector is the library name, a slash, the protocol's own name, a dot, and the method's name: `ours.examples.echo/Echo.EchoString`. Nothing checks that convention; the test below hashes whatever string the header gives.

The rule (`system/lib/wire/include/wire/wire.hpp:113-114`) is: take the SHA-256 of the selector, read its first 8 bytes as a little-endian 64-bit number, and clear the top bit.

From `system/lib/wire/test/ordinals_test.py:46-49`:

```python
def method_ordinal(selector: str) -> int:
    """The ordinal of the method |selector| names."""
    digest = hashlib.sha256(selector.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "little") & 0x7FFF_FFFF_FFFF_FFFF
```

For `ours.examples.echo/Echo.EchoString` this gives `0x6e806bd776386789`, which is the number in `ECHO_STRING` above. On the wire the ordinal is stored little-endian, so its bytes are the first eight bytes of the digest, with the top bit of the last one cleared: here the digest begins `89 67 38 76 d7 6b 80 ee`, and the ordinal's bytes are `89 67 38 76 d7 6b 80 6e`.

Why a number computed from a name:

- Two methods cannot be given one number by hand, and a method's number follows from its name. Renaming the library or the protocol therefore changes every ordinal in it, all at once.
- The top bit is cleared so that no method's ordinal can equal the epitaph's, which has every bit set (`system/lib/wire/include/wire/wire.hpp:66-68`).

Each method is declared as a `wire::Method` with three fields (`system/lib/wire/include/wire/wire.hpp:120-124`): the selector, the ordinal, and the kind. The kind decides the transaction id rule below. Write the declaration as one designated initializer, `.selector` first and `.ordinal` right after it, because the test that checks ordinals finds declarations by that shape and fails on one it cannot read (`system/lib/wire/include/wire/wire.hpp:117-119`; `system/lib/wire/test/ordinals_test.py:58-72`).

The tree has no SHA-256 in C++, so the header carries both the selector and the number, and a Python test recomputes every number from its selector (`system/lib/wire/include/wire/wire.hpp:115-117`). To get the number for a new method, write `.ordinal = 0x0`, run the test, and read the right value from its failure message, which has the form `<header>: <selector> is 0x..., not 0x...` (`system/lib/wire/test/ordinals_test.py:114-118`).

```text
python3 system/lib/wire/test/ordinals_test.py
```

Run this by hand every time you add or change a declaration. Neither the default build nor the `ours-verify` boot check runs it (`system/lib/wire/BUILD.gn:26-27` says it is run by hand), so a wrong number passes the boot check. The test also scans only `system/lib/*/include` and `examples/**/include` (`system/lib/wire/test/ordinals_test.py:37`): a protocol library placed anywhere else is not checked.

The kind of a method and its transaction id:

- A *transaction id* (txid) is a number in the header. For a two-way request the client picks one from 1 to `MAX_TXID` (`0x7FFF'FFFF`), and the server copies it into the reply, so that the client can match the reply to its request. The top bit is reserved (`system/lib/wire/include/wire/wire.hpp:70-72`).
- For a one-way request the txid is 0 (`system/lib/wire/src/wire.cpp:24-29`).
- Echo's two methods are both two-way (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:55-67`). `Directory.Open` of `ours.io` is one-way (`system/lib/ours.io/include/ours_io/ours_io.hpp:196-200`), and so are the three `Add*` methods of `ours.process` (`system/lib/ours.process/include/ours_process/ours_process.hpp:74-94`).

## A message on the wire

Every message is a 16-byte *header*, then a *body*. All numbers are little-endian (`system/lib/wire/include/wire/wire.hpp:100-102`).

| Bytes | Field | Value |
|---|---|---|
| 0 to 3 | `txid` | 0 for a one-way request and an epitaph; 1 to `MAX_TXID` for a two-way request, and the reply carries the same. |
| 4 | `direction` | 0 (`ToServer`) for a request, 1 (`ToClient`) for a reply or an epitaph. |
| 5 and 6 | reserved | Zero. |
| 7 | `magic` | 1. |
| 8 to 15 | `ordinal` | The method's number, or the epitaph's. |

The offsets are pinned by `static_assert`s in `system/lib/wire/test/wire_test.cpp:31-35`, and the struct is `wire::Header` (`system/lib/wire/include/wire/wire.hpp:78-89`).

The *direction* byte is what keeps a reply from ever being read as a request. A reply and a request can have the same size, and without the byte a client that handed a server both ends of one channel could make the server answer its own answers for ever. So a server decodes only `ToServer` messages and a client only `ToClient` ones (`system/lib/wire/include/wire/wire.hpp:46-51`). The *magic* byte is the version of this framing: a header with any other value is refused (`system/lib/wire/include/wire/wire.hpp:57-59`).

The *body* starts at byte 16. It is a run of fields at fixed offsets from the start of the message, with no implicit padding, and at most one trailing array of bytes whose length is the `u32` field just before it (`system/lib/wire/include/wire/wire.hpp:22-26`). A message is exactly as long as its fields, and a trailing array ends the message (`system/lib/ours.io/include/ours_io/ours_io.hpp:41-42`). Fields are read and written with `wire::load` and `wire::store`, which copy through `memcpy` because nothing promises that a channel buffer is aligned (`system/lib/wire/include/wire/wire.hpp:134-152`).

Here is an `EchoString` request for the string `hello` with txid 7, as the library's encoder writes it. Its size is 20 bytes plus the string (`examples/echo/protocol/test/ours_examples_echo_test.cpp:70`).

```text
offset  bytes                      meaning
 0      07 00 00 00                txid 7
 4      00                         direction: ToServer
 5      00 00                      reserved
 7      01                         magic
 8      89 67 38 76 d7 6b 80 6e    ordinal of Echo.EchoString
16      05 00 00 00                size of the string: 5
20      68 65 6c 6c 6f             "hello"
```

An encoder starts the message with `wire::start_request`. It zeroes the bytes it is about to use, so that nothing of an earlier message survives in a reused buffer, writes the header, and returns the front of your buffer. It refuses a txid that breaks the method's rule and a size outside 16 to 4096 with `InvArgs`, and a buffer shorter than the message with `BufferTooSmall` (`system/lib/wire/include/wire/wire.hpp:190-203`, `system/lib/wire/src/wire.cpp:104-132`).

From `examples/echo/protocol/src/ours_examples_echo.cpp:58-70`:

```cpp
auto encode_echo_string(std::span<u8> const bytes, EchoStringRequest const &request) -> Encoded {
    if (request.value.size() > MAX_STRING_LENGTH) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_request(bytes, REQUEST_STRING_OFFSET + request.value.size(),
                                       ECHO_STRING, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    store_string(message, REQUEST_SIZE_OFFSET, request.value);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}
```

The decoder is the mirror image, and it is where a server defends itself. `wire::decode_request_header` checks the header, the ordinal and the txid rule; then the decoder checks its own body: no handles, a string of at most `MAX_STRING_LENGTH`, and a size field that accounts for every byte after it (`examples/echo/protocol/src/ours_examples_echo.cpp:39-55`).

From `examples/echo/protocol/src/ours_examples_echo.cpp:72-85`:

```cpp
auto decode_echo_string(Message const message) -> ustl::Result<EchoStringRequest, Status> {
    auto header = wire::decode_request_header(message, ECHO_STRING);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (!message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto value = load_string(message.bytes, REQUEST_SIZE_OFFSET);
    if (value.is_err()) {
        return ustl::err(value.unwrap_err());
    }
    return ustl::ok(EchoStringRequest{.txid = header->txid, .value = value.unwrap()});
}
```

The program, not the codec, writes the message. Echo's server wraps `channel_write` once.

From `examples/echo/echo_server.cpp:69-73`:

```cpp
auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}
```

What a decoder refuses. Every refusal is `InvArgs` (`system/lib/wire/include/wire/wire.hpp:161-162`).

- `wire::decode_header` refuses a message shorter than 16 bytes or longer than 4096, a `magic` other than 1, a non-zero reserved byte, and a `direction` other than the one expected (`system/lib/wire/src/wire.cpp:49-60`).
- `wire::decode_request_header` also refuses an ordinal other than the method's, and a txid that breaks the method's rule (`system/lib/wire/src/wire.cpp:62-73`).
- Your decoder refuses the rest: a body of the wrong length, a field out of range, the wrong number of handles. Echo's decoders refuse any handle, a string longer than 64 bytes, and trailing bytes (`examples/echo/protocol/src/ours_examples_echo.cpp:72-85`, `:95-104`).

## Replies and statuses

A two-way request gets one reply. A reply is a header with `direction` `ToClient` and the request's txid and ordinal, then a `u32` *status* at byte 16 (`system/lib/wire/include/wire/wire.hpp:31-34`). The status is an `ours::Status` number; the numbers are written out and fixed because they cross the system-call boundary (`system/public/ours/status.hpp:10-15`). A decoder refuses a number past the last one (`system/lib/wire/src/wire.cpp:15-21`).

There are two forms:

- *Ok*: the status is 0 and the method's own fields follow from byte 20. An Ok reply is at least 24 bytes (`system/lib/wire/src/wire.cpp:134-138`).
- *Any other status*: the *status form*, exactly 24 bytes, `{header, u32 status, u32 0}`, and no handles (`system/lib/wire/include/wire/wire.hpp:38-39`, `system/lib/wire/src/wire.cpp:93-100`). A server never sends half an answer next to an error (`system/lib/wire/src/wire.cpp:94-95`).

An Ok `EchoString` reply for `hello`, and a failed one with status 7 (`NoResource`):

```text
Ok reply, 29 bytes
 0      07 00 00 00                txid 7, copied from the request
 4      01                         direction: ToClient
 5      00 00                      reserved
 7      01                         magic
 8      89 67 38 76 d7 6b 80 6e    the request's ordinal
16      00 00 00 00                status: Ok
20      05 00 00 00                size of the string: 5
24      68 65 6c 6c 6f             "hello"

Failed reply, 24 bytes (the status form)
 0      07 00 00 00                txid 7
 4      01                         direction: ToClient
 5      00 00                      reserved
 7      01                         magic
 8      89 67 38 76 d7 6b 80 6e    the request's ordinal
16      07 00 00 00                status: NoResource
20      00 00 00 00                zero
```

The client's decoder tells two kinds of failure apart. If it returns `Err`, the reply is malformed. If it returns `Ok` with a status other than `Ok`, the server answered and said it could not do what was asked (`system/lib/wire/include/wire/wire.hpp:183-185`, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:107-111`).

Reply and body rules:

- The server's encoder takes the txid of the request it answers. Encoding a reply for a one-way method, or with a txid outside 1 to `MAX_TXID`, is `InvArgs` (`system/lib/wire/include/wire/wire.hpp:205-210`).
- A method that returns nothing, like `Stop`, replies with the status form even when the status is Ok; its decoder requires the last four bytes to be zero (`examples/echo/protocol/src/ours_examples_echo.cpp:146-163`).
- A reply that is not Ok has no body and no handles, so the encoder refuses a response next to a failure (`examples/echo/protocol/src/ours_examples_echo.cpp:106-113`).

A request that is well formed but that the server cannot or will not carry out is answered with a status, and the connection stays open. The protocol libraries say so: what is well formed but wrong is the server's to answer (`system/lib/ours.io/include/ours_io/ours_io.hpp:45-50`, `system/lib/ours.process/include/ours_process/ours_process.hpp:55-57`).

## Epitaphs

An *epitaph* is the last message a server writes on a channel before it closes it, saying why ([chapter 1](01-concepts.md#epitaphs)). It is the status form with txid 0 and the ordinal `EPITAPH_ORDINAL`, which has every bit set, and it travels `ToClient` (`system/lib/wire/include/wire/wire.hpp:35-36`, `:66-68`, `:221-231`).

```text
Epitaph NotFound, 24 bytes
 0      00 00 00 00                txid 0
 4      01                         direction: ToClient
 5      00 00                      reserved
 7      01                         magic
 8      ff ff ff ff ff ff ff ff    EPITAPH_ORDINAL
16      04 00 00 00                status: NotFound
20      00 00 00 00                zero
```

An epitaph arrives instead of a reply, and it is used in three places:

- A server that cannot take a connection writes one and closes. Echo does this when it has no free client entry, with `NoResource` (`examples/echo/echo_server.cpp:77-84` and `:141`).
- The root answers a route that leads nowhere this way, with `NotFound`, `AccessDenied` or `BadType` (`system/service/init/realm.cpp:740-743`).
- A failed `Open` in `ours.io` is reported as an epitaph on the new channel, because `Open` has no reply (`system/lib/ours.io/include/ours_io/ours_io.hpp:42-43`).

A message written just before the writer closes is still delivered to the reader first (`kernel/object/include/ours/object/channel_dispatcher.hpp:67-70`), so the epitaph can be read after the channel closed.

A client must test for an epitaph before it decodes a reply. Its txid is 0, which a reply decoder refuses as malformed, so without the test "the server said why it closed" would look like "the server sent a garbage reply" (`system/lib/wire/include/wire/wire.hpp:233-244`). A client reads the header with `wire::decode_header`, asks `wire::is_epitaph`, and only then decodes the reply.

From `system/lib/ours.resource/src/client.cpp:68-81`:

```cpp
    if (auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        header.is_ok() && wire::is_epitaph(header.unwrap())) {
        close_handles();
        auto const epitaph = wire::decode_epitaph(message);
        if (epitaph.is_err()) {
            return ustl::err(Status::InvArgs);
        }
        // An Ok epitaph says nothing about why; it reads as the connection
        // closing, as fsio reads one.
        if (epitaph.unwrap() == Status::Ok) {
            return ustl::err(Status::PeerClosed);
        }
        return ustl::err(epitaph.unwrap());
    }
```

`wire::decode_epitaph` is strict: txid 0, the epitaph ordinal, `ToClient`, exactly 24 bytes, a zero pad, no handles, and a status `ours::Status` names (`system/lib/wire/include/wire/wire.hpp:227-231`, `system/lib/wire/src/wire.cpp:157-173`).

Not every close has an epitaph. When a protocol is closed, as the next section says, an unknown or undecodable message is not answered with one.

## Closed protocols

A protocol is *closed* when its server closes the connection on a message it cannot decode, an unknown method included. Every protocol in this tree is closed: `ours.examples.echo` (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`), `ours.resource` (`system/lib/ours.resource/include/ours_resource/ours_resource.hpp:45-47`), `ours.process` (`system/lib/ours.process/include/ours_process/ours_process.hpp:50-54`) and the server of `ours.io` (`system/lib/ours.io/include/ours_io/ours_io.hpp:45-50`, `system/lib/ioservice/include/ioservice/server.hpp:83-90`).

When a server gets a message its decoder refuses, or whose ordinal it does not know, it:

- writes nothing: no reply and no epitaph;
- closes every handle the message carried, since none was taken;
- closes the channel.

The client then reads `PeerClosed` from its channel, the same status it would read if the server had died.

The reason is in the header. A protocol that wanted an old server to answer "unknown method" to a new client's call would need a bit in the header to mark such a call, and the header has none: byte 4 is the direction (`system/lib/ioservice/src/server.cpp:562-565`). A peer that sends what does not decode is not speaking the protocol, so no answer would help it (`system/lib/ours.io/include/ours_io/ours_io.hpp:45-50`).

Echo's server shows the rule. It decodes the header, compares the ordinal with each method's, and calls that method's decoder. If the ordinal is unknown or the decoder refuses, control falls to the lines that close the message's handles and release the client.

From `examples/echo/echo_server.cpp:177-191`:

```cpp
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_ok() && header->ordinal == ours_examples_echo::ECHO_STRING.ordinal) {
            if (auto const request = ours_examples_echo::decode_echo_string(message);
                request.is_ok()) {
                return echo_string(client, *request);
            }
        } else if (header.is_ok() && header->ordinal == ours_examples_echo::STOP.ordinal) {
            if (auto const request = ours_examples_echo::decode_stop(message); request.is_ok()) {
                stop(client, *request);
            }
        }
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
```

This code runs inside the callback the Loop calls when the client's channel has something to read. The callback answers `Close` here, and the Loop closes the channel. The exact form of the callback is in `system/lib/loop/include/loop/loop.hpp`; this chapter does not describe it.

Why closing is the answer to a mixed-up route: before the protocol type existed, a route that ended at the wrong protocol produced exactly this silent close, and the client read `PeerClosed`, which looks like a server that died (`docs/design/261029/protocol-identity.md:11-17`). That is why the build and the root now compare protocol types.

Which answer a server gives:

| What the server has | What it does |
|---|---|
| A message its decoder refuses (bad header, wrong size, unknown method, field out of range, wrong handle count) | Closes the handles and the channel; writes nothing. |
| A well-formed request it cannot carry out | Replies with the failure status in the status form; the connection stays. |
| A new connection it cannot take | Writes an epitaph with the status, then closes (`examples/echo/echo_server.cpp:141`). |
| The client's end closed | Releases the connection (`examples/echo/echo_server.cpp:154-156`). |

## Handles in messages

A message can carry handles: the second part of `wire::Message` (`system/lib/wire/include/wire/wire.hpp:126-132`). A handle is a number that names an entry in the process's handle table ([handles and channels](01-concepts.md#handles-and-channels)); in code it is an `ours::Uandle` (`system/public/ours/types.hpp:47-72`).

How a handle is laid out:

- In the bytes, a handle is a `u32` field that holds the placeholder `HANDLE_PRESENT`, `0xFFFF'FFFF` (`system/lib/wire/include/wire/wire.hpp:61-64`).
- The handle itself travels in the message's handle array. The array is in the order of the handle fields.
- Every handle field is required. There is no absent value, and an invalid handle is refused (`system/lib/wire/include/wire/wire.hpp:258-261`).

An encoder uses `wire::encode_handle`, a decoder `wire::decode_handle`.

From `system/lib/ours.resource/src/ours_resource.cpp:50-60`:

```cpp
    auto started = wire::start_ok_reply(bytes, wire::STATUS_FORM_SIZE, protocol.get, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status =
            wire::encode_handle(message, REPLY_RESOURCE_OFFSET, handles, 0, reply.resource);
        status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
```

From `system/lib/ours.resource/src/ours_resource.cpp:72-79`:

```cpp
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || message.handles.size() != 1) {
        return ustl::err(Status::InvArgs);
    }
    auto resource = wire::decode_handle(message, REPLY_RESOURCE_OFFSET, 0);
    if (resource.is_err()) {
        return ustl::err(resource.unwrap_err());
    }
    return ustl::ok(GetReply{.status = Status::Ok, .resource = resource.unwrap()});
```

The decoder counts the handles itself. `wire::decode_handle` assumes the caller has checked that the field lies inside the bytes and that the message carries as many handles as it has handle fields (`system/lib/wire/include/wire/wire.hpp:249-253`); here that is `message.handles.size() != 1`. Echo has no handle fields, so its decoders refuse any handle.

Who owns a handle:

- `channel_write` takes every handle you name out of your table, whatever the outcome. On success it is in the message; on failure it is closed (`system/public/ours/syscalls.gni:204-207`).
- A handle that arrives in a message is put into the reader's table by `channel_read` (`kernel/lib/syscall/src/channel.cpp:183-193`). If the reader refuses the message, it must close the handles itself, as Echo's server does (`examples/echo/echo_server.cpp:188-190`) and as a client does (`system/lib/ours.resource/src/client.cpp:63-67`, `:82-86`). A process may have created at most 1024 live handles at a time (`system/public/ours/syscall.hpp:59`), so a leak is not harmless.
- A failed reply carries no handles (`system/lib/wire/src/wire.cpp:96-98`).

Handles appear in `ours.io` (the channel in `Open`, a memory object in `GetBackingMemory`; `system/lib/ours.io/include/ours_io/ours_io.hpp:26-38`), in `ours.process` (the executable and the job in `Launch`, the process in its reply; `system/lib/ours.process/include/ours_process/ours_process.hpp:33-38`), and in `ours.resource` (the resource in a `Get` reply; `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:35-39`).

## How large a message can be

| Limit | Value | Where it is set and enforced |
|---|---|---|
| Bytes in one message, header included | 4096 | `CHANNEL_MAX_MSG_BYTES`, `system/public/ours/syscall.hpp:43`. `channel_write` answers `OutOfRange` past it (`kernel/lib/syscall/src/channel.cpp:107`), and `wire::decode_header` refuses a longer message (`system/lib/wire/src/wire.cpp:51`). |
| Handles in one message | 64 | `CHANNEL_MAX_MSG_HANDLES`, `system/public/ours/syscall.hpp:46`. `channel_write` answers `OutOfRange` past it (`kernel/lib/syscall/src/channel.cpp:72`). |
| Messages waiting on one end | 64 | A write to a full queue answers `NoResource` and consumes the message (`kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`, `:57-61`). |

`wire` restates the first two as `MAX_MESSAGE_BYTES` and `MAX_MESSAGE_HANDLES` (`system/lib/wire/include/wire/wire.hpp:74-76`). Servers keep a buffer of that size for each message they read and one for each they write (`examples/echo/echo_server.cpp:100-108`).

What this means when you design a protocol:

- Give every variable-length field a bound of your own, smaller than the message, state it in the header, and enforce it in the encoder and the decoder. Echo's string is at most 64 bytes (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:48-49`).
- Prove with a `static_assert` that the largest message fits. Echo's is `static_assert(REPLY_STRING_OFFSET + MAX_STRING_LENGTH <= wire::MAX_MESSAGE_BYTES)` at `examples/echo/protocol/src/ours_examples_echo.cpp:24`: its largest reply is 24 + 64 = 88 bytes.
- `ours.io` shows a bound derived from the message size. A read reply carries what is left after the header, the status and the size, 4072 bytes (`system/lib/ours.io/include/ours_io/ours_io.hpp:61-65`), and a path is at most 1024 bytes, which leaves room in a 4 KiB message for `Open`'s other fields (`system/lib/ours.io/include/ours_io/ours_io.hpp:56-59`).
- Do not assume more than 4096. The limit may be raised later without breaking anything, but it may not be lowered (`system/public/ours/syscall.hpp:36-42`).
- For bulk data, send a handle to a memory object instead of the bytes. `File.GetBackingMemory` of `ours.io` does this: its reply holds one handle, `vmo@20` (`system/lib/ours.io/include/ours_io/ours_io.hpp:38`).

## What is versioned and what is not

Versioned: the framing. The `magic` byte is the version of this framing, and a header with another value is refused (`system/lib/wire/include/wire/wire.hpp:57-59`). Status numbers are fixed, because they cross the system-call boundary (`system/public/ours/status.hpp:10-15`), and so are the kernel's size limits in the one direction the comment promises (`system/public/ours/syscall.hpp:36-42`).

Not versioned: a protocol. No message and no manifest entry carries a version for a protocol, and the protocol type is only a name; versions and layouts are explicitly outside the type's job (`docs/design/261029/protocol-identity.md:30`). The tree has no written policy for changing a protocol.

What the rules in this chapter imply for a change, which you should treat as a consequence and not as a promise:

- A new method is a new selector, so a new ordinal. A client that sends it to a server built without it loses its connection, because the server is closed to unknown methods and the header has no bit to soften that.
- Renaming a library, a protocol or a method gives it new ordinals, so the old name's callers lose their connection in the same way.
- Nothing detects a changed layout of a method that keeps its ordinal, except a decoder refusing the size or a field, which also closes the connection. Treat a method's layout as fixed once a client in another build can send it.

Within this tree every peer is built from the same source in one build, so no old binary is left to talk to (`docs/changes/261004/ours-io.md:29` says so about the switch to hashed ordinals).

## Larger protocols to read

Once echo's two methods are clear, read these.

`system/lib/ours.process` is the Launcher, the protocol through which a client has a process started. It has four methods: `AddArgs`, `AddHandles` and `AddNames` are one-way and stateful, each appending to the next process, and `Launch` is two-way and starts it (`system/lib/ours.process/include/ours_process/ours_process.hpp:17-21`, `:74-102`). Its strings travel packed, each ended by a NUL, back to back (`system/lib/ours.process/include/ours_process/ours_process.hpp:43-48`). `Launch` carries two handles in the request and one in the reply, and a failed `Launch` is the status form (`system/lib/ours.process/include/ours_process/ours_process.hpp:33-41`).

`system/lib/ours.resource` holds several protocols that share one shape, `Get`, answered with a handle. Each is a `Protocol` value that pairs the name with its `Get` method (`system/lib/ours.resource/include/ours_resource/ours_resource.hpp:51-56`), and the functions take the `Protocol` as an argument. A decoder for one protocol refuses another's `Get`, because the ordinal differs (`system/lib/ours.resource/src/ours_resource.cpp:30-32`): ordinals are what keep protocols apart on a channel.

## How to add a new protocol library

The steps below build a library for an example protocol `ours.examples.hello` with one method, `Hello.Greet`. Every step has a model in `examples/echo/protocol`.

### 1. Choose where it lives and what it is called

- Next to the program that provides it, in `examples/<program>/protocol`, when only that program's protocol is meant, as Echo's is (`examples/echo/protocol/BUILD.gn:9-12`). Use `system/lib/<library name>` when the system speaks it, as for `system/lib/ours.process`.
- The library name is dotted, as in `ours.examples.hello`; the protocol name is the library name, a dot, and the protocol: `ours.examples.hello.Hello`. The C++ name is `ours_examples_hello`.
- The target is named after its directory, so that `//examples/hello/protocol` is its label. `ours.resource` does the same (`system/lib/ours.resource/BUILD.gn:10-14`).

### 2. Scaffold it with the copier template

`tools/templates/cpp_library` is a copier template for a C++ library with the shape every library here has. Install its dependency once with `python3 -m pip install -r scripts/requirements.txt` (`tools/templates/README.md`; `scripts/requirements.txt` lists `copier`), then copy the template to the new directory (`tools/templates/cpp_library/copier.yml:3`):

```text
python3 -m copier copy tools/templates/cpp_library examples/hello/protocol
```

It asks four questions (`tools/templates/cpp_library/copier.yml:13-33`):

- `name`: the library's name and its include prefix, lower_snake_case starting with a letter. Answer `ours_examples_hello`.
- `namespace`: the C++ namespace; it defaults to `name`.
- `summary`: one line saying what the library is for.
- `gn_path`: the library's GN path without the leading `//`. It defaults to `kernel/lib/<name>`, so answer `examples/hello/protocol`. Nothing checks that it agrees with the directory you copied to.

It writes eight files (the list is what it produces for these answers; the contents are in `tools/templates/cpp_library`):

```text
.clang-format
.gitignore
BUILD.gn
README.md
include/ours_examples_hello/ours_examples_hello.hpp
src/ours_examples_hello.cpp
test/BUILD.gn
test/ours_examples_hello_test.cpp
```

The header and the source are empty shells with the guard and the namespace; the test holds one placeholder that passes. The `.clang-format` names the library's own include prefix as the first include group, which is the reason to use the template and not to copy a library by hand (`tools/templates/cpp_library/copier.yml:5-9`; `tools/templates/cpp_library/.clang-format.jinja:13`). Echo's `.clang-format` is what the template writes for its name.

### 3. Fix the generated BUILD.gn

The template does not know this is a protocol library. Make four changes, and compare with Echo's file.

- Name the `source_set` after the directory, `protocol` here, not after `name`. The generated test depends on `//examples/hello/protocol`, and GN reads that as the target named like the directory, so with the generated name `gn gen` stops with "Unresolved dependencies".
- Add `"//system/lib/wire"` to the `source_set`'s `public_deps`: the messages are `wire::Message` and the methods `wire::Method`, so a user of the codec is a user of the framing.
- Give `headers` its own `public_deps`: `//system/lib/ustl:headers`, `//system/lib/wire:headers` and `//system/public`.
- Add `visibility = [ "*" ]` to both targets, and a `tests` group.

From `examples/echo/protocol/BUILD.gn:13-23`:

```gn
source_set("protocol") {
  sources = [ "src/ours_examples_echo.cpp" ]

  # Its messages are wire::Message and its methods wire::Methods, so a user
  # of the codec is a user of the framing too.
  public_deps = [
    ":headers",
    "//system/lib/wire",
  ]
  visibility = [ "*" ]
}
```

From `examples/echo/protocol/BUILD.gn:28-36`:

```gn
cpp_interface("headers") {
  public = [ "include/ours_examples_echo/ours_examples_echo.hpp" ]
  public_deps = [
    "//system/lib/ustl:headers",
    "//system/lib/wire:headers",
    "//system/public",
  ]
  visibility = [ "*" ]
}
```

From `examples/echo/protocol/BUILD.gn:39-42`:

```gn
group("tests") {
  testonly = true
  public_deps = [ "test:ours_examples_echo($host_toolchain)" ]
}
```

The `test/BUILD.gn` the template writes already has the right shape: a `testonly` executable that depends on the library and on googletest (`examples/echo/protocol/test/BUILD.gn:4-13`). Its dependency label must resolve, which is why the target is named after the directory. GN files you change are formatted with `python3 scripts/format_gn.py <path>`, never with plain `gn format` (CLAUDE.md, "Repo conventions").

### 4. Write the header

Copy the pattern of `ours_examples_echo.hpp`:

- a comment that says what each method is for, writes each message's layout as fields and offsets (`name size@16`), and states that the protocol is closed (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:14-40`);
- the protocol name, as a `std::string_view` constant (`ECHO_NAME`), and your bounds;
- each method as a `wire::Method`, `.selector` first and `.ordinal` right after it. Use `.ordinal = 0x0` for now;
- a struct for each request and each reply;
- an `encode_` and a `decode_` for each request, and for each two-way method an `encode_..._reply` and a `decode_..._reply`.

### 5. Write the source

For each message follow echo's source (`ours_examples_echo.cpp`):

- An encoder checks its own bounds, starts the message with `wire::start_request` or `wire::start_ok_reply`, and stores the fields with `wire::store`. A failed reply is `wire::encode_status_reply`.
- A decoder calls `wire::decode_request_header` or `wire::decode_reply_status`, then checks the body: its exact length, every bound, and the number of handles. It returns `InvArgs` for anything else.
- A reply decoder that sees a status other than `Ok` returns that status inside `Ok`, as echo's does (`examples/echo/protocol/src/ours_examples_echo.cpp:127-135`).

### 6. Write the test

Replace the placeholder in `test/<name>_test.cpp`. Echo's test (`ours_examples_echo_test.cpp`) is the model: for each message, a round trip at its boundary values (empty, longest), and then a list of refusals, each one message with one thing broken. It starts every buffer filled with `0xCC` so that a byte the encoder forgot to write shows (`examples/echo/protocol/test/ours_examples_echo_test.cpp:26-33`).

From `examples/echo/protocol/test/ours_examples_echo_test.cpp:96-103`:

```cpp
    auto refused = [](Message const message) {
        auto const decoded = ours_examples_echo::decode_echo_string(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    // The size must account for every byte after it, and no more.
    auto trailing = good;
    trailing.bytes.push_back('!');
    EXPECT_TRUE(refused(trailing.view()));
```

### 7. Wire the tests into the default build

The host tests that run are the `testonly` executables in the host toolchain that `//:default` builds, found by asking GN each time (`scripts/host_tests.py:5-12`). So the library's `tests` group has to be named in the `default` group of the root `BUILD.gn`. Echo's entry, with the comment above it, is in the root `BUILD.gn`.

From `BUILD.gn:212-214`:

```gn
    # The codec of ours.examples.echo, the protocol //examples/echo provides
    # to the routing checks: each message written, read back and broken.
    "//examples/echo/protocol:tests",
```

Without that line nothing builds your test, and nothing runs it.

### 8. Compute the ordinals and run everything

1. Run `python3 system/lib/wire/test/ordinals_test.py`. It fails on the `0x0`, and its message gives the right number for each selector; paste each one in, and run it again until it says `OK`.
2. Run `python3 .agents/skills/ours-verify/scripts/boot_check.py`. It builds, runs the host tests (yours among them) and boots both dev images (`.agents/skills/ours-verify/SKILL.md`, "Run it").
3. A failing host test can be run alone from the build directory: `cd out/<dir> && ./host/obj/<path>` (`SKILL.md`, "Reading a failure").

The server and its clients then list the library in their `deps`, as `examples/echo/BUILD.gn:24-28` does with `protocol`, and the provider's manifest declares and exposes the protocol name as shown in "Names and the protocol type".

## Where to look next

- [Chapter 4, tree servers](04-tree-servers.md): when the server serves a directory tree and the protocol is `ours.io`, which you do not write.
- [Chapter 5, testing and debugging](05-testing-and-debugging.md): how host tests and the dev image are run, and what a client sees for each failure.
- [Chapter 6, checklist](06-checklist.md): the rules for writing and reviewing a server, including the limits and the closed-protocol rule from this chapter.
- [Chapter 1, concepts](01-concepts.md#epitaphs): the kernel objects this chapter builds on.
- `system/lib/wire/include/wire/wire.hpp`: the framing's full contract, in its comments.

## Sources

- `system/lib/wire/README.md`, `system/lib/wire/BUILD.gn`, `system/lib/wire/include/wire/wire.hpp`, `system/lib/wire/src/wire.cpp`
- `system/lib/wire/test/wire_test.cpp`, `system/lib/wire/test/ordinals_test.py`
- `examples/echo/protocol/` (`BUILD.gn`, `README.md`, the header, the source, `test/`), `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`
- `system/lib/ours.process/include/ours_process/ours_process.hpp`, `system/lib/ours.resource/` (header, `src/ours_resource.cpp`, `src/client.cpp`, `BUILD.gn`), `system/lib/ours.io/include/ours_io/ours_io.hpp`
- `system/lib/ioservice/include/ioservice/server.hpp`, `system/lib/ioservice/src/server.cpp`
- `tools/templates/cpp_library/` (`copier.yml`, the `.jinja` files), `tools/templates/README.md`, `scripts/requirements.txt`, `scripts/host_tests.py`, root `BUILD.gn`
- `system/public/ours/status.hpp`, `system/public/ours/syscall.hpp`, `system/public/ours/types.hpp`, `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/channel.cpp`, `kernel/object/include/ours/object/channel_dispatcher.hpp`
- `system/component_manifest.gni`, `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/realm.cpp`, `system/lib/component/include/component/route.hpp`
- `docs/architecture/component-framework/glossary.md`, `docs/architecture/component-framework/libraries.md`, `docs/design/261004/component-framework.md`, `docs/design/261029/protocol-identity.md`, `docs/changes/261004/ours-io.md`
- `.agents/skills/ours-verify/SKILL.md`

Checked against commit `c2b7a75a` on 2026-10-29.
