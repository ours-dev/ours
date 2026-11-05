# nvme changes

## Title: the NVMe driver: a second block peripheral, polled, on the same block protocol (2026-11-03)

**Files**
- `system/driver/nvme/nvme.cpp` — the driver: ECAM class-match discovery (base 01h / subclass 08h), BAR0 mapped uncached through `MappedMmio`, one page-aligned DMA region sliced into admin queues, identify page, I/O queues and one data page, controller bring-up to CSTS.RDY in the specification's order, Identify Namespace for the geometry, the I/O queue pair created over the admin queues, and `NvmeBlockServer` serving `ours.block.Block` — `start()` runs one request to completion, doorbell and phase poll inline.
- `system/driver/nvme/BUILD.gn` — the component: `user_executable` + manifest using Mmio/Iommu/Route, exposing `ours.block.Block`.
- `kernel/products/image/dev/BUILD.gn` — the nvme child, the Mmio/Iommu/Route offers to it, and the offer of `ours.block.Block` from `#nvme` to routing_checks under the name `ours.block.Nvme` (the offer's `as` rename is how two drivers serve one protocol on two names).
- `system/service/init/test/routing_checks/{BUILD.gn,routing_checks.cpp}` — the `ours.block.Nvme` use (typed, like the echo use), `check_nvme()`: GetInfo as the probe, then a pattern written to the scratch LBA 5 and read back over it and its untouched neighbour; its own skip line and not-run verdict line, as the virtio checks have.
- `scripts/qemu_boot.py` — `NVME_DISK=` attaches `-device nvme,serial=ours0` (drive `if=none,id=nvme0,snapshot=on`), beside `DISK`'s virtio device.
- `system/service/init/pools.hpp` — init's instance pool grows one seat.

**Symptom**
None — a new capability. The machine's second block peripheral behind a real
protocol stack: a client that opens `ours.block.Nvme` reaches QEMU's NVMe
controller with the same GetInfo/Read/Write protocol the virtio disk answers,
and a dual-disk boot runs both chains at once — virtio through GPT/btreefs to
a mounted filesystem, NVMe through its own connection to a pattern
write-read-back.

**Cause**
None — this is a feature, not a fix. The debugging, though, carried the
round's learning value; it is recorded under "How it was found".

**Change**
The bring-up's write order is the specification's, and each step is a
register store through the uncached mapping:

```cpp
regs.write32(NVME_CC, 0);
// ... wait CSTS.RDY falls ...
regs.write32(NVME_AQA, AQA_ACQS_FIELD | AQA_ASQS_FIELD);  // depths, as count-1
regs.write64(NVME_ASQ, dma->phys() + SLICE_ADMIN_SQ);
regs.write64(NVME_ACQ, dma->phys() + SLICE_ADMIN_CQ);
regs.write32(NVME_CC, cc_sizes);                // IOSQES=6, IOCQES=4 before EN
regs.write32(NVME_CC, cc_sizes | CC_ENABLE);
```

The completion's fourth dword is read whole, and its fields named where the
device puts them:

```cpp
/// ... the fourth carries the command id in its low half, the phase at
/// bit 16 -- the bit a polled driver tells a fresh entry from a stale one
/// by, which flips each pass of the ring -- and the status field above.
constexpr auto command_ok(u32 const dw3) -> bool {
    return (dw3 & 0xFFFE'0000u) == 0;
}
```

`submit()` takes the SQE by value and stamps the command id the completion
is matched by — the version before it never wrote the CID into the SQE, so
every command raced as CID 0 (see "How it was found").

**Reference**
- Discovery by class code is PCI's own answer to "the family names the
  driver" — Linux `pci_register_driver`'s `id_table` matching by class, and
  the design's replacement for virtio's vendor/device match.
- The polled completion with the phase bit and the two doorbells is the
  specification's own polling scheme (NVMe 1.4, section 4.10); Linux
  `nvme_poll()` drives the same ring with the same phase discipline.
- The block protocol is ours.block (`.agents/tasks/block-driver-kit/DESIGN.md`),
  the same `block_server::BlockServer` subclass virtio-blk implements; the
  offer's rename is Fuchsia component routing's `as` on an offer.

**Verification**
Full `boot_check.py` runs: dbg PASS (46 passed), rel PASS (44 passed,
7/7 required), diskless and dual-disk (`DISK=` + `NVME_DISK=` in one boot —
virtio chain through GPT/btreefs to the mounted filesystem's file checks,
NVMe chain through GetInfo and the LBA-5 write-read-back, both silent-passing;
`routing_checks: passed`, no not-run lines). clang-format and clang-tidy
clean. The bring-up log names its own progress: `nvme: controller ready,
version 0x10400, doorbell stride 0x4` → `nvme: controller ready, NSZE
0x20000` → `nvme: serving ours.block.Block over 0x20000 blocks` (0x20000 =
131072 blocks = 64 MiB, the file's size). not measured: real-hardware NVMe
(QEMU's model only), and no benchmark — the round is a feature round.

**Deliberately not done**
- AHCI, and virtio multi-disk: later rounds (the design's table).
- Multi-device in one driver: the discovery loop stops at the first
  class-match; a second NVMe controller needs the N-endpoint routing
  refactor, as virtio's would.
- Interrupts: the kernel has no MSI yet (P3 in the interrupt redesign), and
  the polled completion is the specification's own answer — the device's
  interrupt pin is never unmasked.
- The I/O scheduler seat: the slot table is where it lands later (the
  block-server library's own note); MAX_INFLIGHT=1 keeps this round's
  behavior the virtio driver's twin.

**How it was found**
Four bugs stood between "controller ready" and "blocks served", and each
was named by evidence rather than by guessing:

1. **The 7.5-second mirage.** The driver sat "stuck" at CSTS.RDY — but the
   keep-alive lines proved the machine alive, and the CSTS register's
   readback showed the enable had landed (0x460001). The break came from
   running QEMU by hand for 40 seconds: `controller ready` at ~3.6 s. The
   bring-up had never been stuck; boot_check's verdict ends the machine
   at ~8 s, and CAP.TO's ready timer (TO=15 × 500 ms) was reaching its
   deadline at the same wall-clock moment the harness killed the guest.
   The diskless step-3 verification had passed for the same reason it
   could not see this: no device, no ready wait, no window to fall out of.
2. **AQA shifted twice.** The constant carried its own `<< 16`, and the
   write site shifted again — out of u32, so the admin CQ field landed as
   zero: an admin queue with no entries. The readback (AQA reads 0x1) was
   the clue; the readback only became readable once the correct value was
   derived by hand from the specification first.
3. **The completion layout.** After RDY and Identify succeeded, Create I/O
   CQ timed out. The CQE dump (8 raw dwords on the timeout path) showed
   both admin slots carrying completions with DW3 = 0x10000 — decoded
   against QEMU's `hw/nvme` source (the repo's prebuilt QEMU 11.1.1,
   fetched from the same tag): DW3 = CID[15:0] | phase(bit 16) |
   status[31:17], and `cqe.status = (status << 1) | phase` sits in the
   dword's high half. The phase was being read from bit 0 — the CID's low
   bit — and the status never compared at all.
4. **The unstamped CID.** The same dump showed completions whose CID was 0
   for commands submitted as id 1 and 2: `submit()` matched completions by
   a command id it never wrote into the SQE. Identify passed because its
   id was 0 — the same value the unstamped field defaults to.

The method that worked, stated as a rule: dump the raw bytes and read them
against the device model's source, before theorizing about what the device
"must" be doing.

**The user's prediction**
None — the user was not asked to predict for this round; the bring-up
bugs were found and fixed by the session.

**Three questions you should be able to answer**
- Why does a polling driver still need the machine kept alive during
  bring-up, and what keeps it alive once the block service is wired?
- The completion's phase bit moved from "bit 0" to "bit 16" — what does the
  low half of that dword hold, and what did misreading it do to the first
  admin command?
- Root.cm offers `ours.block.Block` from two drivers. What stops a client
  opening `/svc` from seeing two entries, and what names the NVMe one?
