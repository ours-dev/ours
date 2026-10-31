# Protocols: the messages a server and its clients exchange

## What you will be able to do

After this chapter you will be able to:

- say what a protocol is, and which files make one in this repository;
- choose a protocol's name, its methods, and the method number each goes by;
- lay a message out byte by byte, and read one in a hex dump;
- decide, for any message a server receives, whether to answer it, to close the channel, or to say why with an epitaph;
- put a handle in a message, and keep every message within its size limits;
- add a new protocol library to the repository, with its build target and its test.

This chapter is about the messages. The program that answers them is [chapter 2](02-first-server.md); a program that serves a directory tree instead is [chapter 4](04-tree-servers.md). The kernel's words (handle, channel, epitaph, manifest) are defined in [chapter 1](01-concepts.md); each is also explained in a line where it first matters here.

## In short

- A *protocol* is the agreement about the messages on one channel. Here it is a library that the server and every client link: a name, a list of methods, and an encoder and a decoder for each message.
- Its name is the library name, a dot and the protocol's own name: `ours.examples.echo.Echo`.
- Each method has a *selector* (its full name) and a *method number* computed from it; a Python test checks the number.
- A message is a 16-byte header and a flat body at fixed offsets. A failed reply is always the 24-byte status form.
- A decoder checks everything it reads. A message it refuses, or a method it does not know, makes the server close the channel and write nothing: the protocol is *closed*.
- An *epitaph* is the last message a server writes before closing a channel, saying why. A client tests for one before it decodes a reply.
- A handle travels in the message's list of handles; its place in the bytes holds a placeholder.
- A message is at most 4096 bytes and 64 handles. Bound every variable-length field yourself.
- To write a protocol, follow [How to add a new protocol library](#how-to-add-a-new-protocol-library), which comes right after the method numbers.
- A protocol has no version of its own: [What is versioned and what is not](#what-is-versioned-and-what-is-not).

The sections marked *Reference* can be skipped on a first read.

## What a protocol is

A *protocol* is the agreement between a server and its clients about what they write on one channel: which kinds of request exist, what bytes each message holds, and what comes back. A [channel](01-concepts.md#handles-and-channels) is a two-ended pipe; one end writes a *message*, the other end reads it. A message is some bytes and, optionally, some handles.

Words used throughout this chapter:

- *method*: one kind of request, such as `EchoString`.
- *two-way method*: the server sends exactly one *reply* for each request (`wire::MethodKind::TwoWay`).
- *one-way method*: the server sends no reply (`wire::MethodKind::OneWay`).
- *selector*: the string that names a method in full, such as `ours.examples.echo/Echo.EchoString`.
- *method number*: the 64-bit number in a message's header that says which method it calls. It is computed from the selector. The code calls it the *ordinal*, and the header field is `ordinal`: the two words mean one thing, and this guide says *method number* except in identifiers.
- *codec*: the code that turns a C++ struct into the bytes of a message (an *encoder*) and the bytes of a message back into a struct (a *decoder*).

In this repository a protocol is a library of its own, and it holds the protocol's name, its methods and a codec for each message. The server and every client link the same library, so they cannot disagree about a layout. The codecs are written by hand, on top of a framing library called `wire` that holds what every protocol shares: the header, the form of a failed reply, and the epitaph (`system/lib/wire/README.md:3`).

A codec never touches a channel. It works on buffers of bytes that you give it, and the program hands what an encoder returned to `channel_write`. Because it makes no system call it builds on the host, and its tests run there (`system/lib/wire/include/wire/wire.hpp:44-46`).

A decoder is also the server's first line of defence. A server reads what programs it does not trust send it, so every decoder checks everything it reads. The decoders return the repository's result type, `ustl::Result<T, ours::Status>` (`ustl` is explained in [words used elsewhere](01-concepts.md#words-used-elsewhere)): `is_ok()` and `is_err()` ask which it is, `unwrap()` takes the value and `unwrap_err()` the status.

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

It declares each method with `wire::method()`, from the method's selector and its kind. The result is a `wire::Method`, which also holds the method's number (the field `ordinal`); the compiler computes the number from the selector, so no one types it. "Methods and their numbers" below explains how.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:55-56`:

```cpp
inline constexpr auto ECHO_STRING =
    wire::method("ours.examples.echo/Echo.EchoString", wire::MethodKind::TwoWay);
```

It declares a struct for what a message carries, in the form the program uses it.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:65-70`:

```cpp
struct EchoStringRequest {
    u32 txid;
    /// At most MAX_STRING_LENGTH bytes. Decoded, it views the message's
    /// bytes.
    std::string_view value;
};
```

And it declares the functions that turn that struct into a message and back. The encoder writes into a buffer the caller supplies (`bytes`) and returns the `wire::Message` to write; the decoder reads a `wire::Message` and returns the struct, or an error status.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:89-94`:

```cpp
[[nodiscard]]
auto encode_echo_string(std::span<u8> bytes, EchoStringRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_echo_string(wire::Message message) -> ustl::Result<EchoStringRequest, ours::Status>;
```

The header's comment also writes every message's layout as a table of fields and offsets, and states the rules about the string. Do the same for your protocol: the layout is part of the interface, and a reader should not have to read the decoder to learn it.

From `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:26-37`:

```cpp
/// The bodies are flat, at fixed offsets from the start of the message:
///
///   Echo.EchoString   two-way  u32 size@16, the string@20
///   Echo.Stop         two-way  (nothing)
///
/// and their Ok replies, the status at 16 as in every reply:
///
///   Echo.EchoString   u32 size@20, the string@24
///   Echo.Stop         the status form alone
///
/// The string is never absent: it is always there, possibly empty, since a
/// flat body has no absent marker.
```

The notation of the table:

- `u32 size@16` is a 32-bit unsigned field named `size` at byte 16, counted from the first byte of the header, so a body starts at 16. `the string@20` is the string's bytes, right after it. `(nothing)` is a body with no fields.
- `(handle)` after a field, as in `u32 vmo@20 (handle)` in the reply of `File.GetBackingMemory` of `ours.io` (`system/lib/ours.io/include/ours_io/ours_io.hpp:38`), marks a field that stands for a handle ([Handles in messages](#handles-in-messages)). A *VMO* is a block of memory a program can map ([words used elsewhere](01-concepts.md#words-used-elsewhere)).
- An Ok reply has a status at byte 16 first, so its own fields start at 20. "The status form alone" is the 24-byte reply of [Replies and statuses](#replies-and-statuses).

## Names and the protocol type

Three names are involved, and they are easy to confuse.

| Name | Echo's | Used for |
|---|---|---|
| Library name | `ours.examples.echo` | The first part of the protocol name and of every selector. |
| Protocol name | `ours.examples.echo.Echo` (`ECHO_NAME`) | Manifests, `/svc/<name>` in a client, `svc/<name>` in a provider's outgoing directory. |
| C++ name | `ours_examples_echo` | The include prefix, the namespace, the test target. |

The protocol name is the library name, a dot, and the protocol's own name; the C++ name is the library name with its dots made underscores, because a C++ name cannot hold a dot. A component finds the protocol under its protocol name in its `/svc`, and a provider registers the same name with its outgoing directory in one call; a route may rename it on the way ([`/svc` and the outgoing directory](01-concepts.md#svc-and-the-outgoing-directory)).

The larger libraries follow the same rule: `ours.process`, whose Launcher is the protocol with which a client has a process started, is named `ours.process.Launcher`, and `ours.resource`, whose protocols let `init` hand out kernel resources, has `ours.resource.Debug`, `ours.resource.Irq` and others.

Where this is stated: the rule, `system/lib/ours.process/include/ours_process/ours_process.hpp:59-61` and `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:49-50`; the C++ name, `system/lib/ours.resource/BUILD.gn:10-13`; the name's two uses and the rename, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:42-47`; the registration call, `examples/echo/echo_server.cpp:303`.

A component's manifest says which protocols it provides, hands up and needs ([components and manifests](01-concepts.md#components-and-manifests)). Echo's is a `component_manifest()` target: `program` names the program it ships with, and `manifest` declares the protocol (`capabilities`) and exposes it to the component's parent (`expose`). The target opens with its name, who may depend on it, and its program:

From `examples/echo/BUILD.gn:33-35`:

```gn
component_manifest("echo.cm") {
  visibility = [ "//kernel/products/image/*" ]
  program = ":echo_server"
```

and the manifest follows, up to the end of the target:

From `examples/echo/BUILD.gn:36-49`:

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
}
```

The protocol name is typed here and in `ECHO_NAME` (`std::string_view`). GN cannot read a C++ constant, so the two are kept equal by hand; [chapter 6](06-checklist.md) lists the check.

The *protocol type* is a string that says which protocol an entry speaks. It is written `type` in a manifest and, when left out, equals the entry's own name (`system/component_manifest.gni:109-111`). It exists because an `offer` may rename a capability with `as`, after which the name no longer says what the protocol is. The dev image's root manifest offers echo's protocol to `routing_checks` under another name (`kernel/products/image/dev/BUILD.gn:186-191`), so `routing_checks` states in its `use` which protocol it means. Here is that entry, from the `use` list of `routing_checks`' manifest (a `component_manifest("routing_checks.cm")` target whose `use = [` opens at `system/service/init/test/routing_checks/BUILD.gn:53`):

From `system/service/init/test/routing_checks/BUILD.gn:85-88`:

```gn
      {
        protocol = "ours.test.Echo"
        type = "ours.examples.echo.Echo"
      },
```

A route must end at a capability of the type its use states, and this is checked twice. The build checks every route in the whole component tree under a manifest (`system/component_manifest.gni:62-65`). `init`, the program that runs the root, checks again on every Open: it logs a line containing `reaches another protocol` and answers the Open with the epitaph `BadType` on the channel the client passed (`system/service/init/realm.cpp:204-215` and `:792-795`; `system/lib/component/include/component/route.hpp:291-305`).

The type is only a name. It says nothing about a protocol's layout: two protocols that share a type string are not told apart (`system/lib/component/include/component/route.hpp:352-354`). So choose a name that is yours, and keep it for one layout.

## Methods and their numbers

A method's number follows from its selector: you write the selector, and the compiler computes the number. Every request names the method it calls by that number, in bytes 8 to 15 of its header.

By the repository's convention a selector is the library name, a slash, the protocol's own name, a dot, and the method's name: `ours.examples.echo/Echo.EchoString`. Nothing checks that convention; the compiler hashes whatever string the header gives.

A codec header declares a method by its selector and its kind, and nothing else. This is `EchoString`, which shows the whole declaration (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:55-56`):

```cpp
inline constexpr auto ECHO_STRING =
    wire::method("ours.examples.echo/Echo.EchoString", wire::MethodKind::TwoWay);
```

`wire::method()` is `consteval`: the compiler runs it where it is written, as the header is compiled, so a method costs the program no code and no start-up work, and a selector that is not a constant string does not compile (`system/lib/wire/include/wire/wire.hpp:149-154`). It writes the selector's number into the method with `wire::ordinal_of()`, which is the rule (`system/lib/wire/include/wire/wire.hpp:139-147`): take the SHA-256 of the selector, read its first 8 bytes as a little-endian 64-bit number, and clear the top bit.

```cpp
[[nodiscard]]
consteval auto ordinal_of(std::string_view const selector) -> u64 {
    auto const digest = sha256(selector);
    auto ordinal = u64{0};
    for (auto i = usize{0}; i < 8; ++i) {
        ordinal |= u64{digest[i]} << (8 * i);
    }
    return ordinal & ORDINAL_MASK;
}
```

You never run this yourself. It is here so that you can see what the number is. For `ours.examples.echo/Echo.EchoString` it gives `0x6e806bd776386789`. On the wire the number is stored little-endian, so its bytes are the first eight bytes of the digest, with the top bit of the last one cleared: here the digest begins `89 67 38 76 d7 6b 80 ee`, and the number's bytes are `89 67 38 76 d7 6b 80 6e`.

Why a number computed from a name:

- Nobody types it, so it cannot be mistyped, and it cannot be copied from another method without notice. Two methods of a protocol that end with the same number do not compile, because a header ends with a `static_assert` over its methods (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:61-63`):

```cpp
// A server tells the methods of Echo apart by ordinal, so no two may share
// one.
static_assert(wire::all_distinct(ECHO_STRING, STOP), "two Echo methods have the same ordinal");
```

- The selector is part of the interface. Renaming the library, the protocol or a method changes the number of every method concerned, all at once.
- The top bit is cleared so that no method's number can equal the epitaph's, which has every bit set (`system/lib/wire/include/wire/wire.hpp:69-71`).

Adding a method:

1. Declare it with `wire::method(selector, kind)`, as `ECHO_STRING` above is declared, after the others. The kind is `wire::MethodKind::TwoWay` or `OneWay`.
2. Name it in the header's `static_assert(wire::all_distinct(...))`, with the others.
3. Build. There is no number to compute and nothing to run: the compiler hashes the selector, and the `static_assert` stops the build if two methods have the same number. With one selector written twice in a header of `hello`, the build stopped with ``static assertion failed due to requirement 'wire::all_distinct(GREET, STOP)': two Hello methods have the same ordinal``.

The kind of a method decides its transaction id:

- A *transaction id* (txid) is a number in the header. For a two-way request the client picks one from 1 to `MAX_TXID` (`0x7FFF'FFFF`), and the server copies it into the reply, so that the client can match the reply to its request. The top bit is reserved (`system/lib/wire/include/wire/wire.hpp:78-80`).
- For a one-way request the txid is 0 (`system/lib/wire/src/wire.cpp:24-29`).
- Echo's two methods are both two-way. `Directory.Open` of `ours.io`, the protocol of files and directories, is one-way (`system/lib/ours.io/include/ours_io/ours_io.hpp:26-27`), and so are the three `Add*` methods of `ours.process` (`system/lib/ours.process/include/ours_process/ours_process.hpp:74-85`).

## How to add a new protocol library

The steps below build a library for an example protocol `ours.examples.hello` with one method, `Hello.Greet`. `Greet` has the shape of `EchoString`: it takes a string and answers with a string. Every step has a model in `examples/echo/protocol`, and says what changes when your protocol does not have echo's shape.

### 1. Choose where it lives and what it is called

- Next to the program that provides it, in `examples/<program>/protocol`, when only that program's protocol is meant, as Echo's is (`examples/echo/protocol/BUILD.gn:9-12`). Use `system/lib/<library name>` when the system speaks it, as for `system/lib/ours.process`.
- The library name is dotted, as in `ours.examples.hello`; the protocol name is the library name, a dot, and the protocol: `ours.examples.hello.Hello`. The C++ name is `ours_examples_hello`.
- The GN target is named after its directory, so that `//examples/hello/protocol` is its label. `ours.resource` does the same (`system/lib/ours.resource/BUILD.gn:10-14`). Step 3 says why.

### 2. Scaffold it with the copier template

[Chapter 2](02-first-server.md) takes a shorter way for its first server: it copies `examples/echo/protocol` and renames it, so it starts from a `BUILD.gn` that already has the right shape. Use the template when the library you want is not a copy of one that exists.

`tools/templates/cpp_library` is a copier template for a C++ library with the shape every library here has. Install its dependency once ([the Python setup](00-setup.md#python) says how), then copy the template to the new directory (`tools/templates/cpp_library/copier.yml:3`):

```text
python3 -m copier copy tools/templates/cpp_library examples/hello/protocol
```

It asks four questions (`tools/templates/cpp_library/copier.yml:13-33`):

- `name`: the library's name and its include prefix, lower_snake_case starting with a letter. Answer `ours_examples_hello`.
- `namespace`: the C++ namespace; it defaults to `name`.
- `summary`: one line saying what the library is for.
- `gn_path`: the library's GN path without the leading `//`. It defaults to `kernel/lib/<name>`, so answer `examples/hello/protocol`. Nothing checks that it agrees with the directory you copied to.

It writes eight files (the list is what it produces for these answers):

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

The header and the source are empty shells with the include guard and the namespace; the test holds one placeholder that passes. The `.clang-format` names the library's own include prefix as the first include group, which is the reason to use the template and not to copy a library by hand (`tools/templates/cpp_library/copier.yml:5-9`; `tools/templates/cpp_library/.clang-format.jinja:13`). Echo's `.clang-format` is what the template writes for its name.

### 3. Fix the generated BUILD.gn

The template does not know this is a protocol library, so its `BUILD.gn` needs changes. With `summary` answered "The ours.examples.hello protocol.", the template writes this `BUILD.gn` (its source is `tools/templates/cpp_library/BUILD.gn.jinja`):

```gn
import("//build/cpp/cpp_interface.gni")

# The ours.examples.hello protocol.
source_set("ours_examples_hello") {
  sources = [ "src/ours_examples_hello.cpp" ]
  public_deps = [ ":headers" ]
}

# The headers are a target of their own so that a dependant needing only the
# declarations does not pull in the sources -- see //build/cpp/cpp_interface.gni.
cpp_interface("headers") {
  public = [ "include/ours_examples_hello/ours_examples_hello.hpp" ]
}
```

A `source_set` compiles its `sources` into whatever depends on it, and passes its `public_deps` on to its dependants. `cpp_interface("headers")` is the repository's template for a target of headers only (`build/cpp/cpp_interface.gni:15-18`, `:57-58`): it publishes the library's `include/` directory to its dependants and compiles nothing, so a user that needs only the declarations depends on `:headers` and does not pull in the sources.

Make five changes:

1. Name the `source_set` after the directory, `protocol` here, not after `name`. The generated test depends on `//examples/hello/protocol`, GN reads that as the target named like the directory, and so with the generated name `gn gen` stops with `Unresolved dependencies`, naming `//examples/hello/protocol:protocol` as the target it needs.
2. Add `"//system/lib/wire"` to the `source_set`'s `public_deps`: the messages are `wire::Message` and the methods `wire::Method`, so a user of the codec is a user of the framing.
3. Give `headers` its own `public_deps`: `//system/lib/ustl:headers`, `//system/lib/wire:headers` and `//system/public`, which the header's includes need.
4. Add `visibility = [ "*" ]` to both targets. GN's default is the same (`gn help visibility`), so the line only says it aloud; the repository's other protocol libraries write it.
5. Add a `tests` group that names the test target in the host toolchain. Step 7 uses it.

Echo's finished file has all five.

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

From `examples/echo/protocol/BUILD.gn:28-42`:

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

# Host unit tests; wired into //:default.
group("tests") {
  testonly = true
  public_deps = [ "test:ours_examples_echo($host_toolchain)" ]
}
```

The `test/BUILD.gn` the template writes already has the right shape: a `testonly` executable that depends on the library and on googletest (`examples/echo/protocol/test/BUILD.gn:4-13`). Its dependency label is the one that change 1 makes resolve.

The repository formats a GN file you change with `python3 scripts/format_gn.py <path>`, and never with plain `gn format`, which sorts the lists in the file and so changes the order of the link line and of the include path (`scripts/format_gn.py:6-11`).

### 4. Write the header

Copy the pattern of `ours_examples_echo.hpp`, which this chapter has shown piece by piece:

- a comment that says what each method is for, writes each message's layout as fields and offsets (`name size@16`, as in the table above), and states that the protocol is closed (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:14-40`);
- the protocol name, as a `std::string_view` constant (`ECHO_NAME`), and your bounds;
- each method as `wire::method(selector, kind)`, as "Methods and their numbers" shows, and a `static_assert(wire::all_distinct(...))` over them;
- a struct for each request and each reply;
- an `encode_` and a `decode_` for each request, and for each two-way method an `encode_..._reply` and a `decode_..._reply`.

Where your method differs from `EchoString`:

- A method with no arguments still has a request struct, which holds the txid: `StopRequest` (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:81-83`).
- A reply that can fail has a struct with a `status` and the method's fields, which are there only when the status is Ok: `EchoStringReply` (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:72-79`).
- A method that returns nothing has no reply struct. Its encoder takes the status and its decoder returns it: `encode_stop_reply` and `decode_stop_reply` (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:116-121`).

### 5. Write the source

For each message follow echo's source (`examples/echo/protocol/src/ours_examples_echo.cpp`). An encoder checks its own bounds, starts the message with `wire::start_request` and stores the fields with `wire::store`.

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

A decoder calls `wire::decode_request_header`, then checks the body: its exact length, every bound, and the number of handles. It returns `InvArgs` for anything else.

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

The reply encoder is the same with `wire::start_ok_reply`, which also writes the Ok status. This is the Ok path of `encode_echo_string_reply`; its first lines send a failure through `wire::encode_status_reply` instead.

From `examples/echo/protocol/src/ours_examples_echo.cpp:117-125`:

```cpp
    auto started =
        wire::start_ok_reply(bytes, REPLY_STRING_OFFSET + reply.response.size(), ECHO_STRING, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    store_string(message, REPLY_SIZE_OFFSET, reply.response);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}
```

The reply decoder calls `wire::decode_reply_status` and, for a status other than `Ok`, returns that status inside `Ok`, so that the caller can tell a server's failure from a malformed reply (`examples/echo/protocol/src/ours_examples_echo.cpp:127-135`). What the helpers do is in [A message on the wire](#a-message-on-the-wire) and [Replies and statuses](#replies-and-statuses).

### 6. Write the test

Replace the placeholder in `test/<name>_test.cpp`. Echo's test (`examples/echo/protocol/test/ours_examples_echo_test.cpp`) is the model: for each message, a round trip at its boundary values (empty, longest), and then a list of refusals, each one message with one thing broken. It starts every buffer filled with `0xCC` so that a byte the encoder forgot to write shows (`examples/echo/protocol/test/ours_examples_echo_test.cpp:26-33`).

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

The host tests that run are the `testonly` executables in the host toolchain that `//:default` builds (`scripts/host_tests.py:5-12`). So the library's `tests` group, change 5 of step 3, has to be named in the `default` group of the root `BUILD.gn`. Echo's entry is `"//examples/echo/protocol:tests"` there (`BUILD.gn:222-224`).

Without that line nothing builds your test, and nothing runs it. [Adding a host test of your own](05-testing-and-debugging.md#adding-a-host-test-of-your-own) shows the line and how to check that your test is found.

### 8. Run everything

1. Run the verify script, `python3 .agents/skills/ours-verify/scripts/boot_check.py`. It builds, runs the host tests (yours among them) and boots the images; [Build and boot the dev image](README.md#build-and-boot-the-dev-image) says what it prints.
2. A failing host test can be run alone from the build directory. The binary lies under `host/obj/` at its target's path: for echo's codec, `cd out/bringup.x86 && ./host/obj/examples/echo/protocol/test/ours_examples_echo`.

The server and its clients then list the library in their `deps`, as `examples/echo/BUILD.gn:24-28` does with `protocol`, and the provider's manifest declares and exposes the protocol name, spelled as `ECHO_NAME` is, as shown in "Names and the protocol type".

## A message on the wire

*Reference.* Read this when you write a codec by hand, read a hex dump, or wonder why a decoder refused a message; the recipe above works without it.

Every message is a 16-byte *header*, then a *body*. All numbers are little-endian.

| Bytes | Field | Value |
|---|---|---|
| 0 to 3 | `txid` | 0 for a one-way request and an epitaph; 1 to `MAX_TXID` for a two-way request, and the reply carries the same. |
| 4 | `direction` | 0 (`ToServer`) for a request, 1 (`ToClient`) for a reply or an epitaph. |
| 5 and 6 | reserved | Zero. |
| 7 | `magic` | 1. |
| 8 to 15 | `ordinal` | The method's number, or the epitaph's. |

The *direction* byte is what keeps a reply from ever being read as a request. A reply and a request can have the same size, and without the byte a client that handed a server both ends of one channel could make the server answer its own answers for ever. So a server decodes only `ToServer` messages and a client only `ToClient` ones. The *magic* byte is the version of this framing: a header with any other value is refused.

The *body* starts at byte 16. It is a run of fields at fixed offsets from the start of the message, with no implicit padding, and at most one trailing array of bytes whose length is the `u32` field just before it. A message is exactly as long as its fields, and a trailing array ends the message. Fields are read and written with `wire::load` and `wire::store`, which copy through `memcpy` because nothing promises that a channel buffer is aligned.

Here is an `EchoString` request for the string `hello` with txid 7, as the library's encoder writes it. Its size is 20 bytes plus the string.

```text
offset  bytes                      meaning
 0      07 00 00 00                txid 7
 4      00                         direction: ToServer
 5      00 00                      reserved
 7      01                         magic
 8      89 67 38 76 d7 6b 80 6e    method number of Echo.EchoString
16      05 00 00 00                size of the string: 5
20      68 65 6c 6c 6f             "hello"
```

The program, not the codec, writes the message: it hands the encoder's bytes and handles to `channel_write`, which [chapter 1](01-concepts.md#handles-and-channels) shows in echo's one-function wrapper.

An encoder starts the message with `wire::start_request`. It zeroes the bytes it is about to use, so that nothing of an earlier message survives in a reused buffer, writes the header, and returns the front of your buffer. It refuses a txid that breaks the method's rule and a size outside 16 to 4096 with `InvArgs`, and a buffer shorter than the message with `BufferTooSmall`.

What a decoder refuses. Every refusal is `InvArgs`.

- `wire::decode_header` refuses a message shorter than 16 bytes or longer than 4096, a `magic` other than 1, a non-zero reserved byte, and a `direction` other than the one expected.
- `wire::decode_request_header` also refuses a method number other than the method's, and a txid that breaks the method's rule.
- Your decoder refuses the rest: a body of the wrong length, a field out of range, the wrong number of handles. Echo's decoders refuse any handle, a string longer than 64 bytes, and trailing bytes.

Where this is stated: `system/lib/wire/include/wire/wire.hpp:25-29` (the body's shape), `:49-62` (the direction and magic bytes), `:86-97` (`wire::Header`), `:108-110` (little-endian), `:186-204` (`load` and `store`) and `:242-255` (the helpers of an encoder); `system/lib/wire/src/wire.cpp:49-73` (what `decode_header` and `decode_request_header` refuse) and `:104-132`; `system/lib/wire/test/wire_test.cpp:34-38` (the offsets, pinned by `static_assert`s); `examples/echo/protocol/test/ours_examples_echo_test.cpp:70` (the request's size); `examples/echo/protocol/src/ours_examples_echo.cpp:39-55` and `:95-104` (what echo's decoders refuse).

## Replies and statuses

A two-way request gets one reply. A reply is a header with `direction` `ToClient` and the request's txid and method number, then a `u32` *status* at byte 16. The status is an `ours::Status` number; the numbers are written out and fixed because they cross the system-call boundary. A decoder refuses a number past the last one.

There are two forms:

- *Ok*: the status is 0 and the method's own fields follow from byte 20. An Ok reply is at least 24 bytes.
- *Any other status*: the *status form*, exactly 24 bytes, `{header, u32 status, u32 0}`, and no handles. A server never sends half an answer next to an error.

An Ok `EchoString` reply for `hello`, and a failed one with status 7 (`NoResource`):

```text
Ok reply, 29 bytes
 0      07 00 00 00                txid 7, copied from the request
 4      01                         direction: ToClient
 5      00 00                      reserved
 7      01                         magic
 8      89 67 38 76 d7 6b 80 6e    the request's method number
16      00 00 00 00                status: Ok
20      05 00 00 00                size of the string: 5
24      68 65 6c 6c 6f             "hello"

Failed reply, 24 bytes (the status form)
 0      07 00 00 00                txid 7
 4      01                         direction: ToClient
 5      00 00                      reserved
 7      01                         magic
 8      89 67 38 76 d7 6b 80 6e    the request's method number
16      07 00 00 00                status: NoResource
20      00 00 00 00                zero
```

The client's decoder tells two kinds of failure apart. If it returns `Err`, the reply is malformed. If it returns `Ok` with a status other than `Ok`, the server answered and said it could not do what was asked.

Reply and body rules:

- The server's encoder takes the txid of the request it answers. Encoding a reply for a one-way method, or with a txid outside 1 to `MAX_TXID`, is `InvArgs`.
- A method that returns nothing, like `Stop`, replies with the status form even when the status is Ok; its decoder requires the last four bytes to be zero (`examples/echo/protocol/src/ours_examples_echo.cpp:146-163`).
- A reply that is not Ok has no body and no handles, so the encoder refuses a response next to a failure (`examples/echo/protocol/src/ours_examples_echo.cpp:106-113`).

A request that is well formed but that the server cannot or will not carry out is answered with a status, and the channel stays open. The protocol libraries say so: what is well formed but wrong is the server's to answer.

Where this is stated: `system/lib/wire/include/wire/wire.hpp:34-42` (the two forms), `:235-237` (the two outcomes of a reply decoder) and `:257-262` (the server's encoder); `system/lib/wire/src/wire.cpp:15-21` (the refusal past the last status), `:93-100` and `:134-138`; `system/public/ours/status.hpp:10-15` (fixed status numbers); `system/lib/ours.io/include/ours_io/ours_io.hpp:45-50` and `system/lib/ours.process/include/ours_process/ours_process.hpp:55-57` (what is the server's to answer).

## Epitaphs

An *epitaph* is the last message a server writes on a channel before it closes it, saying why; [chapter 1](01-concepts.md#epitaphs) explains what it is for and what it is not. This section is its bytes and its uses.

It is the status form with txid 0 and the method number `EPITAPH_ORDINAL`, which has every bit set, and it travels `ToClient` (`system/lib/wire/include/wire/wire.hpp:38-39`, `:69-71`).

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

- A server that cannot take a client writes one and closes. Echo does this when it has no free client entry (its record of one client; see [Closed protocols](#closed-protocols)), with `NoResource` (`examples/echo/echo_server.cpp:168`; the helper is `close_with_epitaph`, `:77-84`, shown in [chapter 1](01-concepts.md#epitaphs)).
- `init` answers a route that leads nowhere this way, with the status of the failed walk: `NotFound` when something on the route is missing, `BadType` when it ends at another protocol (`system/service/init/realm.cpp:792-796`).
- A failed `Open` in `ours.io` is reported as an epitaph on the new channel, because `Open` has no reply (`system/lib/ours.io/include/ours_io/ours_io.hpp:42-43`).

A message written just before the writer closes is still delivered to the reader first (`kernel/object/include/ours/object/channel_dispatcher.hpp:67-70`), so the epitaph can be read after the channel closed.

A client must test for an epitaph before it decodes a reply. Its txid is 0, which a reply decoder refuses as malformed, so without the test "the server said why it closed" would look like "the server sent a garbage reply" (`system/lib/wire/include/wire/wire.hpp:285-296`). A client reads the header with `wire::decode_header`, asks `wire::is_epitaph`, and only then decodes the reply.

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

`wire::decode_epitaph` is strict: txid 0, the epitaph's method number, `ToClient`, exactly 24 bytes, a zero pad, no handles, and a status `ours::Status` names (`system/lib/wire/include/wire/wire.hpp:279-283`, `system/lib/wire/src/wire.cpp:157-173`).

Not every close has an epitaph. When a protocol is closed, as the next section says, an unknown or undecodable message is not answered with one.

## Closed protocols

A protocol is *closed* when its server closes the channel on a message it cannot decode, an unknown method included. Every protocol in this repository is closed: `ours.examples.echo`, `ours.resource`, `ours.process` and the server of `ours.io`.

When a server gets a message its decoder refuses, or whose method number it does not know, it:

- writes nothing: no reply and no epitaph;
- closes every handle the message carried, since none was taken;
- closes the channel.

The client then reads `PeerClosed` from its channel, the same status it would read if the server had died.

Why no answer? The code that decides it says: the header has no bit that would mark a call as one an older server may answer "unknown" to (byte 4 is the direction), so every unknown call closes the channel (`system/lib/ioservice/src/server.cpp:570-574`). The layouts show why the mark would be needed: a reply carries the request's method number and txid, so an "unknown method" answer would look exactly like that method's own reply with a failure status, and a client that does know the method could not tell the two apart. And a peer that sends what does not decode is not speaking the protocol, so no answer would help it (`system/lib/ours.io/include/ours_io/ours_io.hpp:45-50`).

Echo's server shows the rule. It keeps one *client entry* for each connected client: an object of the class `Client` that holds the client's channel and is the `loop::Waiter` the Loop calls when that channel has something to read. Each such channel also holds one Loop *slot*, the table entry that holds the wait ([the Loop and its slots](01-concepts.md#the-loop-and-its-slots)). The Loop calls the entry's `on_signals`, which hands the call to `serve` (`examples/echo/echo_server.cpp:113-115`; [chapter 2](02-first-server.md) shows how a client's channel is given to the Loop). `serve` decodes the header, compares the method number with each method's, and calls that method's decoder. If the number is unknown or the decoder refuses, control falls to the lines that close the message's handles and release the client.

From `examples/echo/echo_server.cpp:212-226`:

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

`release` frees the client entry and answers `Close`, so the Loop closes the channel and frees the slot.

From `examples/echo/echo_server.cpp:173-176`:

```cpp
    static auto release(Client &client) -> loop::Action {
        client.channel = Uandle::Invalid;
        return loop::Action::Close;
    }
```

A route that ends at the wrong protocol produces exactly this close when the client calls a method the server does not know, and the client reads `PeerClosed`, which looks like a server that died. That is why the build and `init` compare protocol types, as "Names and the protocol type" says: they turn that case into a build error or an epitaph `BadType`.

Which answer a server gives:

| What the server has | What it does |
|---|---|
| A message its decoder refuses (bad header, wrong size, unknown method, field out of range, wrong handle count) | Closes the handles and the channel; writes nothing. |
| A well-formed request it cannot carry out | Replies with the failure status in the status form; the channel stays open. |
| A new client it cannot take | Writes an epitaph with the status, then closes the channel (`examples/echo/echo_server.cpp:168`). |
| The client's end closed | Frees the client entry; the Loop closes the channel (`examples/echo/echo_server.cpp:189-191`). |

Where this is stated: the rule in each protocol's header, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`, `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:45-47`, `system/lib/ours.process/include/ours_process/ours_process.hpp:50-54` and `system/lib/ours.io/include/ours_io/ours_io.hpp:45-50`; the server of `ours.io`, `system/lib/ioservice/include/ioservice/server.hpp:84-91`; the message that arrives before a close, `kernel/object/include/ours/object/channel_dispatcher.hpp:67-70`.

## Handles in messages

A message can carry handles: the second part of `wire::Message`. A handle is a number that names an entry in the process's handle table ([handles and channels](01-concepts.md#handles-and-channels)); in code it is an `ours::Uandle`.

How a handle is laid out:

- In the bytes, a handle is a `u32` field that holds the placeholder `HANDLE_PRESENT`, `0xFFFF'FFFF`.
- The handle itself travels in the message's handle array. The array is in the order of the handle fields.
- Every handle field is required. There is no absent value, and an invalid handle is refused.

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

The decoder counts the handles itself. `wire::decode_handle` assumes the caller has checked that the field lies inside the bytes and that the message carries as many handles as it has handle fields; here that is `message.handles.size() != 1`. Echo has no handle fields, so its decoders refuse any handle.

Who owns a handle:

- `channel_write` takes every handle you name out of your table, whatever the outcome. On success it is in the message; on failure it is closed (`system/public/ours/syscalls.gni:204-207`).
- A handle that arrives in a message is put into the reader's table by `channel_read` (`kernel/lib/syscall/src/channel.cpp:183-193`). If the reader refuses the message, it must close the handles itself, as Echo's server does (`examples/echo/echo_server.cpp:223-225`) and as a client does (`system/lib/ours.resource/src/client.cpp:63-67`, `:82-86`). A process may have created at most 1024 live handles at a time, wherever they now are (`system/public/ours/syscall.hpp:48-59`), so a leak is not harmless.
- A failed reply carries no handles.

Where this is stated: `system/lib/wire/include/wire/wire.hpp:64-67` (the placeholder), `:178-184` (`wire::Message`) and `:301-313` (`encode_handle`, `decode_handle`); `system/lib/wire/src/wire.cpp:96-98` (a failed reply carries no handles).

Handles appear in `ours.io`, the protocol of files and directories (the channel in `Open`, a VMO in `GetBackingMemory`; `system/lib/ours.io/include/ours_io/ours_io.hpp:26-38`), in `ours.process`, the Launcher (the executable and the job in `Launch`, the process in its reply; `system/lib/ours.process/include/ours_process/ours_process.hpp:33-38`), and in `ours.resource`, the protocols that hand out kernel resources (the resource in a `Get` reply; `system/lib/ours.resource/include/ours_resource/ours_resource.hpp:35-39`).

## How large a message can be

*Reference.* Read the table when you hit a limit; read the advice under it before you design a message.

| Limit | Value | Where it is set and enforced |
|---|---|---|
| Bytes in one message, header included | 4096 | `CHANNEL_MAX_MSG_BYTES`, `system/public/ours/syscall.hpp:43`. `channel_write` answers `OutOfRange` past it (`kernel/lib/syscall/src/channel.cpp:107`), and `wire::decode_header` refuses a longer message (`system/lib/wire/src/wire.cpp:51`). |
| Handles in one message | 64 | `CHANNEL_MAX_MSG_HANDLES`, `system/public/ours/syscall.hpp:46`. `channel_write` answers `OutOfRange` past it (`kernel/lib/syscall/src/channel.cpp:72`). |
| Messages waiting on one end | 64 | A write to a full queue answers `NoResource` and consumes the message (`kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`, `:57-61`). |

`wire` restates the first two as `MAX_MESSAGE_BYTES` and `MAX_MESSAGE_HANDLES` (`system/lib/wire/include/wire/wire.hpp:82-84`). A server keeps a buffer of that size for the message it reads and one for the reply it builds (`examples/echo/echo_server.cpp:130-135`).

What this means when you design a protocol:

- Give every variable-length field a bound of your own, smaller than the message, state it in the header, and enforce it in the encoder and the decoder. Echo's string is at most 64 bytes (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:48-49`).
- Prove with a `static_assert` that the largest message fits. Echo's is `static_assert(REPLY_STRING_OFFSET + MAX_STRING_LENGTH <= wire::MAX_MESSAGE_BYTES)` at `examples/echo/protocol/src/ours_examples_echo.cpp:24`: its largest reply is 24 + 64 = 88 bytes.
- `ours.io` shows a bound derived from the message size. A read reply carries what is left after the header, the status and the size, 4072 bytes (`system/lib/ours.io/include/ours_io/ours_io.hpp:61-65`), and a path is at most 1024 bytes, which leaves room in a 4 KiB message for `Open`'s other fields (`system/lib/ours.io/include/ours_io/ours_io.hpp:56-59`).
- Do not assume more than 4096. The limit may be raised later without breaking anything, but it may not be lowered (`system/public/ours/syscall.hpp:37-42`).
- For bulk data, send a handle to a VMO instead of the bytes. `File.GetBackingMemory` of `ours.io` does this: its reply holds one handle, `vmo@20` (`system/lib/ours.io/include/ours_io/ours_io.hpp:38`).

## What is versioned and what is not

*Reference.* Read this before you change a protocol that a program built elsewhere may speak.

Versioned: the framing. The `magic` byte is the version of this framing, and a header with another value is refused (`system/lib/wire/include/wire/wire.hpp:60-62`). Status numbers are fixed, because they cross the system-call boundary (`system/public/ours/status.hpp:10-15`), and so are the kernel's size limits in the one direction the comment promises (`system/public/ours/syscall.hpp:37-42`).

Not versioned: a protocol. No message and no manifest entry carries a version for a protocol, and the protocol type is only a name (`system/lib/component/include/component/route.hpp:352-354`). This guide found no written policy in the repository for changing a protocol.

What the rules in this chapter imply for a change, which you should treat as a consequence and not as a promise:

- A new method is a new selector, so a new method number. A client that sends it to a server built without it loses its channel, because the server is closed to unknown methods and the header has no bit to soften that.
- Renaming a library, a protocol or a method gives it new method numbers, so the old name's callers lose their channel in the same way.
- Nothing detects a changed layout of a method that keeps its number, except a decoder refusing the size or a field, which also closes the channel. Treat a method's layout as fixed once a client in another build can send it.

Within one dev image the server and its clients are built together from one checkout, so none of them is older than the others; these rules matter when they are built apart.

## Larger protocols to read

Once echo's two methods are clear, read these.

`system/lib/ours.process` is the Launcher, the protocol through which a client has a process started. It has four methods: `AddArgs`, `AddHandles` and `AddNames` are one-way and stateful, each appending to the next process, and `Launch` is two-way and starts it (`system/lib/ours.process/include/ours_process/ours_process.hpp:17-21`, `:74-95`). Its strings travel packed, each ended by a NUL, back to back (`system/lib/ours.process/include/ours_process/ours_process.hpp:43-48`). `Launch` carries two handles in the request and one in the reply, and a failed `Launch` is the status form (`system/lib/ours.process/include/ours_process/ours_process.hpp:33-41`).

`system/lib/ours.resource` holds several protocols that share one shape, `Get`, answered with a handle. Each is a `Protocol` value that pairs the name with its `Get` method (`system/lib/ours.resource/include/ours_resource/ours_resource.hpp:51-56`), and the functions take the `Protocol` as an argument. A decoder for one protocol refuses another's `Get`, because the method number differs (`system/lib/ours.resource/src/ours_resource.cpp:30-32`): method numbers are what keep protocols apart on a channel.

## Where to look next

- [Chapter 2, the first server](02-first-server.md): the program that answers these messages.
- [Chapter 4, tree servers](04-tree-servers.md): when the server serves a directory tree and the protocol is `ours.io`, which you do not write.
- [Chapter 5, testing and debugging](05-testing-and-debugging.md): how host tests and the dev image are run, and what a client sees for each failure.
- [Chapter 6, checklist](06-checklist.md): the rules for writing and reviewing a server, including the limits and the closed-protocol rule from this chapter.
- [Chapter 1, concepts](01-concepts.md#epitaphs): the kernel objects this chapter builds on.
- `system/lib/wire/include/wire/wire.hpp`: the framing's full contract, in its comments.

## Sources

- `system/lib/wire/README.md`, `system/lib/wire/BUILD.gn`, `system/lib/wire/include/wire/wire.hpp`, `system/lib/wire/src/wire.cpp`
- `system/lib/wire/test/wire_test.cpp`, `system/lib/wire/include/wire/sha256.hpp`
- `examples/echo/protocol/` (`BUILD.gn`, the header, the source, `test/`), `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`
- `system/lib/ours.process/include/ours_process/ours_process.hpp`, `system/lib/ours.resource/` (header, `src/ours_resource.cpp`, `src/client.cpp`, `BUILD.gn`), `system/lib/ours.io/include/ours_io/ours_io.hpp`
- `system/lib/ioservice/include/ioservice/server.hpp`, `system/lib/ioservice/src/server.cpp`
- `tools/templates/cpp_library/` (`copier.yml`, the `.jinja` files), `scripts/host_tests.py`, `scripts/format_gn.py`, `build/cpp/cpp_interface.gni`, root `BUILD.gn`
- `system/public/ours/status.hpp`, `system/public/ours/syscall.hpp`, `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/channel.cpp`, `kernel/object/include/ours/object/channel_dispatcher.hpp`
- `system/component_manifest.gni`, `kernel/products/image/dev/BUILD.gn`, `system/service/init/test/routing_checks/BUILD.gn`, `system/service/init/realm.cpp`, `system/lib/component/include/component/route.hpp`

Checked against commit `10dd8149` on 2026-10-30.
