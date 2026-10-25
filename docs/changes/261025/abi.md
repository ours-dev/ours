# abi changes

## Title: a port packet can be a page request: PacketType::PageRequest and its payload (2026-10-25)

**Files**
- `system/public/ours/port.hpp` — `PacketType::PageRequest = 9`, `PageRequestCommand { Read = 0, Complete = 1 }`, `PacketPageRequest`, and `page_request` in `PortPacket`'s union, each layout pinned by `static_assert`

**Symptom**
None, step 7 of `.agents/tasks/user-pager/`. A pager learns what to supply from packets on its port, and the ABI had no packet for that: the proxy of step 9 would have nothing to fill in, and a user pager nothing to read.

**Cause**
None, an addition. The header is shared by the kernel and every program, so the layout is settled before anything on either side writes or reads it.

**Change**
As Zircon has it, value for value, so that the numbers mean what they mean there:

```cpp
enum class PacketType : u32 { User = 0, SignalOne = 1, Interrupt = 7, PageRequest = 9 };
enum class PageRequestCommand : u16 { Read = 0, Complete = 1 };
struct PacketPageRequest {          // 32 bytes, like every member of the union
    PageRequestCommand command;     // 0
    u16 flags;                      // 2, none defined: zero
    u32 reserved0;                  // 4
    u64 offset;                     // 8, bytes of the VMO, page aligned
    u64 length;                     // 16
    u64 reserved1;                  // 24
};
```

The packet's key is the key given to `pager_create_vmo`, so a pager with several VMOs on one port tells them apart by it. `Read` asks for a range a thread is waiting on; `Complete` is the last packet a VMO sends, once it is detached or destroyed, with offset and length zero. `PortPacket` stays 48 bytes, and `page_request` sits at 16 with the other payloads.

Left out: Zircon's third command, `ZX_PAGER_VMO_DIRTY` (2), which asks a pager to allow a write; dirty tracking is a non-goal of the task (DESIGN.md). Number 8 stays unassigned, as in Zircon.

**Reference**
Zircon `ZX_PKT_TYPE_PAGE_REQUEST` (0x09), `zx_packet_page_request_t` and `ZX_PAGER_VMO_READ`/`ZX_PAGER_VMO_COMPLETE` in `system/public/zircon/syscalls/port.h`.

**Verification**
- Build: the `static_assert`s on `PacketPageRequest`'s size and every offset, and on `page_request`'s offset in `PortPacket`, hold for both the kernel and the programs.
- `port.test` passes; `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Nothing produces or reads a PageRequest packet yet: not exercised until steps 9 and 10.

**Deliberately not done**
- `PageRequestCommand::Dirty`: no dirty tracking.
- Updating the packet list in `docs/design/260924/userspace-filesystem.md`, an accepted design that already predates Interrupt.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- How does a pager with several VMOs on one port know which VMO a request is for?
- Why is every member of PortPacket's union exactly 32 bytes?
- What does a Complete packet tell the pager, and when is it sent?
