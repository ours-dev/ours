# Writing a server for ours

A developer's guide to writing a server for the ours kernel: a program that other programs reach over channels, that answers their requests, and that runs in the dev image (the image the build boots; [Build and boot](#build-and-boot-the-dev-image) defines it). It is written for someone who has never seen this kernel.

## The shortest path

A first server needs only part of this guide. Read these, in this order, and treat the rest as reference:

1. This README, three sections: [Status of this guide](#status-of-this-guide), [Before you start](#before-you-start) and [Build and boot the dev image](#build-and-boot-the-dev-image) (about 900 words).
2. [00-setup.md](00-setup.md) (about 1,700 words): what `prebuilt/` must hold. Then run the command of "Build and boot the dev image" once, so that you know what a pass looks like.
3. [02-first-server.md](02-first-server.md): the tutorial, in levels. It sends you into [01-concepts.md](01-concepts.md) one piece at a time, with a "Read now" link and a size at the level that needs it. Before level 0: handles and channels, ports and signals, and the Loop and its slots (about 2,100 words). At level 1: components and manifests (about 1,200). At level 2: `/svc` and the outgoing directory, where a server's memory comes from, and epitaphs (about 1,900 in all).

That is about 7,900 words to a `hello` that the root starts (the end of level 1), about 12,200 to one that answers a client (level 3) and about 13,400 to the end of the tutorial. Chapters 3 to 6 are reference. Chapter 3 is for a second method or a protocol of a different shape, chapter 5 for a boot that fails, chapter 6 for the review of a server, and chapter 4 only to understand a tree server.

## What you will be able to do

By the end of the guide you can:

- say what a server is here, and which kernel objects it stands on;
- read `examples/echo`, the small server that ships in the repository, and know what every part of it is for;
- write a protocol server of your own: define its protocol, write the program, and declare it so that the root starts it and routes clients to it;
- test the server on the host and boot it in the dev image, and read the log when a route fails;
- review a server, your own or someone else's, against a checklist;
- understand how the root's `/boot` and every component's outgoing directory are built, and use the tree library in a host test (a tree server of your own cannot yet be reached by another component; see [Status of this guide](#status-of-this-guide)).

## Who this is for

You know C++ and Git. You have never seen this kernel. You want to write a server and run it in the dev image.

The guide stops at servers: components that answer requests on channels, whether they provide a protocol or serve a directory tree. It does not cover writing a kernel object, the hardware side of a driver, or adding a system call.

## What a server is

In this guide a *server* is a component with a program that answers requests on channels. A component is a program together with the file that declares what it provides and needs (its *manifest*); [01-concepts.md](01-concepts.md#components-and-manifests) explains both. A *client* is a program that sends a server requests.

There are two kinds of server, and the guide uses these two names for them:

- A *protocol server* answers a protocol of its own on each client's channel. `examples/echo` is one. You can write one and run it in the dev image today.
- A *tree server* serves a directory tree: files and directories a client opens by path. The root's `/boot` is one. You can build one and test it on the host, but another component cannot yet reach a tree server of yours: a manifest cannot route a directory that a component provides.

The kinds are not exclusive in what they are built from. A protocol server's outgoing directory, the directory through which clients find it, is itself a small tree, which the library class `ioservice::Outgoing` builds for it. What makes a server one kind or the other is what its clients see: calls of a protocol, or a tree they open by path.

The word "server" alone means the program. `ioservice::Server`, in code font, is a library class that tree servers are built from.

## The running example

`examples/echo` is a protocol server of about 300 lines. It provides one protocol, `ours.examples.echo.Echo`, with two methods: `EchoString` answers with the string it was sent, and `Stop` answers Ok and exits.

- `examples/echo/echo_server.cpp`: the program.
- `examples/echo/protocol/`: the library that encodes and decodes the protocol's messages, with its tests.
- `examples/echo/BUILD.gn`: the program's build target and its manifest, `echo.cm`.

The dev image runs it on every boot. The root starts `echo` as a child, and another child, `routing_checks`, reaches it, sends `EchoString`, then `Stop`, and checks that the next attempt to reach it answers NotFound. So what you read here is what runs, and a boot that passes has exercised it.

## Status of this guide

Two limits apply to everything below.

- **The repository does not install its build tools.** The build needs a `prebuilt/` directory holding clang, gn, ninja, QEMU and GRUB. Git ignores it, and nothing in the repository downloads or assembles it for you. [00-setup.md](00-setup.md) says what it must hold and where each piece on the machine the project is built on came from, for a macOS host on Apple silicon. No other host is described.
- **A tree server of your own cannot yet be reached.** A manifest cannot route a directory that a component provides, so no other component can find a tree server that you write. [04-tree-servers.md](04-tree-servers.md) explains how the root's `/boot` and every outgoing directory work, and how to test a tree on the host; it does not give you a tree server that runs in the dev image.

## Before you start

You need Git, Python 3, and the tools in `prebuilt/`: the build and the boot script run the ones in the checkout's `prebuilt/` directory (clang, gn, ninja, QEMU and GRUB) and never look for one on your `PATH`. The script does not check that they are there or say where to get them: if one is missing, the command below fails at the step that needs it. [00-setup.md](00-setup.md) lists each piece and where the project's own machine got it, and has commands that check what you have. Do that first.

The reference host is macOS on Apple silicon. Nothing in this guide has been checked on another host.

## Build and boot the dev image

The *dev image* is `kernel.dev.obi`, a boot image: a single `.obi` file that holds the kernel and the files user space starts from. It runs `init`, the root, which starts the components that `meta/root.cm` lists: `routing_checks`, `echo`, the shell and the others. Your server runs in this image. The script calls it `rel`. The other image the script boots, `kernel.dev.dbg_1.obi` (`dbg`), is the debug image, built with the kernel's assertions on: it starts the shell directly with no root, so no component starts there.

One command builds the kernel and the user programs, runs the host tests and boots both images. It is the verify script; run it from the root of the checkout:

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py
```

What it does:

- It builds `out/bringup.x86` with `prebuilt/ninja`. A full build takes about 40 seconds with a cold compiler cache and about 6 with a warm one.
- It boots the two images side by side under QEMU, an emulator that runs the image as a virtual PC (`scripts/qemu_boot.py` starts it through GRUB, a boot loader, as real hardware would boot), and stops each as soon as its verdict is in. Booting takes about 25 seconds end to end.
- While they boot it runs the host tests.
- It prints one line per image and one for the host tests, and a last line saying where the serial logs are. Exit code 0 means everything passed.

A passing run prints this. It is a real run; the directory in each path is shortened to `…`, and the times and the counts differ from run to run and grow as checks are added:

```text
build      ok (…/out/bringup.x86, 0.5s)
dbg        PASS  shell finished at 5.6s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end
host-tests PASS  148 passed, 0 failed · 0.7s · log …/host-tests.log
logs       …/{dbg,rel}.log
```

Read it like this:

- `build` says the build finished and how long it took. Here nothing needed building; your first build takes the longer times above.
- `dbg` and `rel` are the verdicts on the two images. `PASS` is the verdict. `passed`, `skipped` and `failed` count the shell's self-checks, and `skipped` are the ones that do not apply to that image. `0 error line(s)` says nothing was logged at error level, and `reached: end` that the boot got to the shell's last line.
- `6/6 required` is only on `rel`: the lines the image must print, all seen. They include `routing_checks: passed` and `init: echo exited with 0`, which is the evidence that `echo` ran, answered a client and was stopped. The other four come from the root and from the keyboard driver.
- `host-tests` runs the tests that need no kernel.

A `FAIL` in place of `PASS` comes with the reason. The script's documentation, `.agents/skills/ours-verify/SKILL.md`, has a table "Reading a failure" that says what each message means and what to do next.

The options you are likely to want:

- `--only rel` boots the dev image alone.
- `--no-build` boots what is already built.
- `--out out/<name>` builds in a directory of your own (it is configured on first use); two builds must not share one.
- `--no-tests` skips the host tests, for a quick loop. Run the full command before you call something done.

The script boots a fixed set of components. How to add a server of your own to the dev image and to its root manifest is chapter 2's subject.

## Chapters

- [00-setup.md](00-setup.md): the tools the build needs in `prebuilt/`, where each one comes from, and how to check them. Do this before the build above.
- [01-concepts.md](01-concepts.md): the ideas everything else rests on, in the order a reader needs them: handles and channels, ports and signals, the Loop and its slots, where a server's memory comes from, components and manifests, `/svc` and the outgoing directory, and epitaphs. Chapter 2 sends you into it one piece at a time, with the size of each; the other chapters link back into it.
- [02-first-server.md](02-first-server.md): a tutorial that copies `examples/echo` into a server called `hello` and grows it in five levels. Level 0 chooses the kind of server and defines its protocol; level 1 has the root start it; level 2 lets a client reach it (2a: the program serves; 2b: the manifests and the client's check); level 3 makes it answer a method; level 4 makes it keep its limits; level 5 makes every boot check it. Each level says which part of chapter 1 to read before it, and how to tell it worked; the finished result is in the tree as `examples/hello`, to compare your copy with.
- [03-protocols.md](03-protocols.md): how to define a protocol, with its methods, message layout and replies, and how to create the library that holds it. Chapter 2 copies echo's library for its first protocol; come here for a second method or a protocol of a different shape.
- [04-tree-servers.md](04-tree-servers.md): when to serve a directory tree instead of a protocol, and how a tree server answers an Open. A tree server of your own cannot yet be reached by another component, so the chapter is for understanding the root's `/boot` and testing a tree on the host.
- [05-testing-and-debugging.md](05-testing-and-debugging.md): testing a server's logic on the host, booting it, and reading what a failing route prints.
- [06-checklist.md](06-checklist.md): a checklist for writing and reviewing a server, grouped by the phase you are in.

## How to read it

Chapter 2 is the way in, and [The shortest path](#the-shortest-path) says what to read before it. Chapters 3 to 6 are reference: read each when a task needs it, not before.

- To write your first server, follow chapter 2 level by level. It links the parts of chapter 1 that each level needs, and chapter 5 when a boot fails. If you would rather read chapter 1 whole, do it in order: each section uses only what the sections before it explained.
- Chapter 3 when you need a second method or a protocol of a different shape. Chapter 2 copies echo's library for its first protocol, and says to skip chapter 3's recipe for it.
- To understand a tree server, or to test a tree on the host, read chapter 4.
- Before you ask anyone to review a server, go through chapter 6: its ten rules, then "What a first version needs".

Keep `examples/echo` open while you read. Every chapter explains a mechanism with a snippet copied exactly from a real file, and names the file and the line (`examples/echo/echo_server.cpp:69`). Each chapter ends with the list of files it was written from, and the commit it was checked against: the lines it quotes are true of that commit, and may have moved since.

Words with a meaning of their own here (handle, channel, port, signals, Loop, slot, component, manifest, capability, protocol, use, offer, expose, root, epitaph) are explained where they first appear, or linked to [01-concepts.md](01-concepts.md). The last section of that chapter, "Words used elsewhere", gives a line each to a few more (VMO, built-in, `ort`, `ustl`, vDSO, obi). The longer list is the [component framework glossary](../architecture/component-framework/glossary.md).

### What to skip on a first pass

- Chapter 4, when you write a protocol server. The one part of it you may want is how memory is declared, and chapter 1 has that in "Where a server's memory comes from".
- In chapter 3, the sections "A message on the wire" and "How large a message can be": the byte layout, until you need to read a dump.
- In chapter 5, the detail of "What went wrong: what a client sees", until a route fails.
- In chapter 6, everything after "The ten rules that matter most" and "What a first version needs", until you review.

## Where to look next

If `prebuilt/` is empty, start with [00-setup.md](00-setup.md). Then follow [The shortest path](#the-shortest-path).

## Sources

- `.agents/skills/ours-verify/scripts/boot_check.py` and the documentation named above: the command, its options, the images, the lines the dev image must print, and the form of its output.
- `scripts/qemu_boot.py`: how QEMU and GRUB boot an image.
- `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`: the running example.
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn`: which program each image starts, and the dev image's root manifest.
- `system/service/init/test/routing_checks/routing_checks.cpp`: what the client of `echo` checks.
- `docs/architecture/component-framework/glossary.md`: the words.

Checked against commit `561571b7` on 2026-10-30.
