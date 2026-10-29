# Writing a server for ours

A developer's guide to writing a server for the ours kernel: a program that other programs reach over channels, that answers their requests, and that runs in the dev image. It is written for someone who has never seen this kernel.

## Status of this guide

This guide is a draft, and an outside reader cannot yet follow it from start to finish. What is missing:

- Chapter 2, the step-by-step tutorial that takes a server from an empty program to one that the root starts and a client reaches, is not written. It waits for the way the Loop is given a client's channel to settle, so that it teaches the final form.
- Building needs a `prebuilt/` directory of tools (clang, gn, ninja, QEMU and GRUB). The tree does not yet say how to obtain them, and the only host the build is documented on is macOS. See "Build and boot the dev image" below.
- A tree server of your own cannot yet be reached by another component, because a manifest cannot route a directory that a component provides. Chapter 4 explains how the root's own `/boot` works, and what a tree server is built from.

Until chapter 2 exists, the way to write a first protocol server is to copy `examples/echo` to a directory of your own, rename it, define your protocol by chapter 3's recipe, and boot it with the edits to the root manifest that chapter 1 describes. Chapters 4, 5 and 6 are reference: read chapter 5 when a boot fails and chapter 6 before review.

## What you will be able to do

By the end of the guide you can:

- say what a server is here, and which kernel objects it stands on;
- read `examples/echo`, the small server that ships in the tree, and know what every part of it is for;
- define the protocol a server speaks, write the server, and declare it so that the root starts it and routes clients to it;
- test the server on the host and boot it in the dev image, and read the log when a route fails;
- review a server, your own or someone else's, against a checklist.

## Who this is for

You know C++ and Git. You have never seen this kernel. You want to write a server and run it in the dev image.

The guide stops at servers: components that answer requests on channels, whether they provide a protocol or serve a directory tree. It does not cover writing a kernel object, the hardware side of a driver, or adding a system call.

## What a server is

In this guide a *server* is a component with a program that answers requests on channels. A component is a program together with the file that declares what it provides and needs; [01-concepts.md](01-concepts.md#components-and-manifests) explains both.

There are two kinds, and the guide uses these two names for them:

- A *protocol server* answers a protocol of its own on each client's channel. `examples/echo` is one.
- A *tree server* serves a directory tree: files and directories a client opens by path. The root's `/boot` is one.

The word "server" alone means the program. `ioservice::Server`, in code font, is a library class that tree servers are built from.

## Chapters

- [01-concepts.md](01-concepts.md): the ideas everything else rests on, in the order a reader needs them: handles and channels, ports and signals, the Loop and its slots, components and manifests, `/svc` and the outgoing directory, and epitaphs. Read it first; the other chapters link back into it.
- 02-first-server.md (in preparation): a staged tutorial that grows an empty skeleton into a tested, booted server.
- [03-protocols.md](03-protocols.md): how to define a protocol, with its methods, message layout and replies, and how to create the library that holds it.
- [04-tree-servers.md](04-tree-servers.md): when to serve a directory tree instead of a protocol, and how a tree server answers an Open.
- [05-testing-and-debugging.md](05-testing-and-debugging.md): testing a server's logic on the host, booting it, and reading what a failing route prints.
- [06-checklist.md](06-checklist.md): a checklist for writing and reviewing a server, grouped by the phase you are in.

## How to read it

Read chapter 1 once, in order: each section uses only what the sections before it explained. After that:

- To write your first server, go on to chapter 2 when it exists, and read chapter 3 beside it.
- To serve files or a directory tree, read chapter 4 as well.
- Once you have something to run, read chapter 5.
- Before you ask anyone to review a server, go through chapter 6.

Keep `examples/echo` open while you read. Every chapter explains a mechanism with a snippet copied exactly from a real file, and names the file and the line (`examples/echo/echo_server.cpp:69`). Each chapter ends with the list of files it was written from, and the commit it was checked against: the lines it quotes are true of that commit, and may have moved since.

Words with a meaning of their own here (handle, channel, port, signals, Loop, slot, component, manifest, capability, protocol, use, offer, expose, root, epitaph) are explained where they first appear, or linked to [01-concepts.md](01-concepts.md). The longer list is the [component framework glossary](../architecture/component-framework/glossary.md).

## The running example

`examples/echo` is a protocol server of about 280 lines. It provides one protocol, `ours.examples.echo.Echo`, with two methods: `EchoString` answers with the string it was sent, and `Stop` answers Ok and exits.

- `examples/echo/echo_server.cpp`: the program.
- `examples/echo/protocol/`: the library that encodes and decodes the protocol's messages, with its tests.
- `examples/echo/BUILD.gn`: the program's build target and its manifest, `echo.cm`.

The dev image runs it on every boot. The root starts `echo` as a child, and another child, `routing_checks`, reaches it, sends `EchoString`, then `Stop`, and checks that the next attempt to reach it answers NotFound. So what you read here is what runs, and a boot that passes has exercised it.

## Build and boot the dev image

The one command that builds the kernel and the user programs, runs the host tests and boots both images is the `ours-verify` skill's script. Run it from the root of the checkout:

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py
```

What it does:

- It builds `out/bringup.x86` with `prebuilt/ninja`. A full build takes about 40 seconds with a cold compiler cache and about 6 with a warm one.
- It boots two images side by side under QEMU: `kernel.dev.dbg_1.obi` (the script calls it `dbg`) and `kernel.dev.obi` (the script calls it `rel`), and stops each as soon as its verdict is in. Booting takes about 25 seconds end to end.
- While they boot it runs the host tests.
- It prints one line per image and one for the host tests, each giving its name and then `PASS` or `FAIL`, and a last line saying where the serial logs are. Exit code 0 means everything passed.

Your server runs in `kernel.dev.obi`, the image this guide means by the dev image: it starts `init`, the root, and the root starts the components. `kernel.dev.dbg_1.obi` starts the shell directly, with no root, so no component starts there. A passing `rel` line is also the evidence that `echo` ran: the script requires the lines `routing_checks: passed` and `init: echo exited with 0` before it accepts that image.

The options you are likely to want:

- `--only rel` boots the dev image alone.
- `--no-build` boots what is already built.
- `--out out/<name>` builds in a directory of your own (it is configured on first use); two builds must not share one.
- `--no-tests` skips the host tests, for a quick loop. Run the full command before you call something done.

If the script prints `FAIL`, the table "Reading a failure" in `.agents/skills/ours-verify/SKILL.md` says what each message means and what to do next.

Two limits of what this section can promise:

- The script runs the tools in the checkout's `prebuilt/` directory: clang, gn, ninja, QEMU and GRUB. The skill documents building and booting on a macOS host with them, and nothing else. Git ignores `prebuilt/`, and the tree does not yet say how to fill it, so this guide cannot either.
- The script boots a fixed image. How to add a server of your own to it, and to its root manifest, is chapter 2's subject.

## Where to look next

Start with [01-concepts.md](01-concepts.md).

## Sources

- `.agents/skills/ours-verify/SKILL.md` and `.agents/skills/ours-verify/scripts/boot_check.py`: the command, its options, the images, the lines the dev image must print.
- `examples/echo/echo_server.cpp`, `examples/echo/BUILD.gn`, `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`: the running example.
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn`: which program each image starts, and the dev image's root manifest.
- `system/service/init/test/routing_checks/routing_checks.cpp`: what the client of `echo` checks.
- `docs/architecture/component-framework/glossary.md`: the words.

Checked against commit `94a62227` on 2026-10-29.
