# protocols

Every wire contract between OS components in one library: one include root
(`<prot/...>`), one C++ namespace (`prot`) with a sub-namespace per protocol
family, and one wire framing (`//system/lib/wire`) under all of it. The
families, each named by its sub-namespace and by the wire name's second word:

- `prot::block` — `ours.block.Block`, the one thing a disk is: blocks of a
  fixed size, read and written by number.
- `prot::io` — `ours.io`, files and directories: the Open/Read/Write/
  Enumerate protocol everything in a namespace is served over, with the
  rights vocabulary (`Flags`, `Operations`).
- `prot::resource` — `ours.resource.*`, the hardware roots (Mmio, Iommu,
  Ioport, Irq, Debug, Sampling) a driver takes through its namespace.
- `prot::acpi` — `ours.acpi.Route`, the route service's firmware answers
  (ISA numbers, the ECAM window).
- `prot::fshost` — `ours.fshost.Admin`, the filesystem host's mount control.
- `prot::process` — `ours.process.Launcher`, how a program becomes a process.

## What a family carries

The codec (`<prot/<family>.hpp>`): the method table (selectors and
ordinals), the request/reply structs, and the hand-written encode/decode
pairs, bytes and handle values alone — no system call, so it builds in every
toolchain, the host one included, where its tests run. Three families with
per-connection plumbing add a client header (`<prot/<family>_client.hpp>`):
the txid counter, the buffers, the wait and the epitaph check over one
connected channel, making system calls and so user-toolchain only.

## The namespace rule

`prot` is an OS-layer namespace on purpose. `ours::` is the kernel ABI's —
the vDSO entries and the types the syscalls speak (`ours::Status`,
`ours::Uandle`, `ours::Rights`) — and only kernel-level things carry it. A
protocol is a contract between components; it speaks the kernel's types but
lives outside the kernel's namespace. The wire names keep the platform's
`ours.` family prefix, the role "fuchsia.*" has in Fuchsia: the C++ binding
namespace does not determine it.

## The abstraction bar

A family's header names the contract and nothing else: no device types (a
disk's driver is virtio or NVMe and the protocol cannot tell), no
driver-framework types (ddk, loop, ioservice), no kernel internals beyond
the ABI types above. The client opens its channel from component plumbing
(fsio), so even the connection's how stays out of the library.
