# init outstanding repairs

## 1. The five resource servers' pools lie in init's `.data`, 22 KiB of its file that is nearly all zeros

**Severity**
Low: a size cost, not a fault. Init's file in the BootFS is about 22 KiB larger than it was, and nothing behaves differently.

**Symptom**
Since `ResourceConnection` became a `loop::Waiter` (`docs/changes/261029/init.md`, the title about the Resource and Launcher connections), each `ResourceServer::Storage` is no longer all zeros, and a `constinit` object with any non-zero byte is written into the image whole. `llvm-nm -S -C` on `init` shows the five storages (`DEBUG_RESOURCE_STORAGE`, `IRQ_RESOURCE_STORAGE`, `IOPORT_RESOURCE_STORAGE`, `SAMPLING_RESOURCE_STORAGE`, `ROOT_JOB_STORAGE`) as type `b` (`.bss`), 4448 bytes each, before, and `d` (`.data`), 4480 bytes each, after. `llvm-size -A`: `.data` 28928 to 51360 bytes, `.bss` 77664 to 55424. The RW `LOAD` segment's file size (`llvm-readelf -l`) goes from 0x7100 to 0xc8a0, and init stripped with `llvm-objcopy --strip-all` from 154,384 to 177,544 bytes. Nearly all of it is the five servers' 4 KiB `request_bytes` buffers, which hold nothing until a message arrives.

**Cause**
The vptr. A connection that is a `Waiter` holds a pointer to its vtable, which is an address and so not zero. A global lives in `.bss` only when every byte of it is zero, and the `Storage` is one object, so the 4 KiB of message buffers beside the four connections go into the file with them. It is the same mechanism as `docs/repairs/261004/init.md` item 9, which records it for the Launcher's pool (a `LauncherOps` vptr), and the first option of that item's "Change" is the same fix. The two items are independent: either can be taken alone.

**Change**
Options for the user, none needed today:
- move each server's message buffers out of `ResourceServer::Storage` into a `.bss` global of their own, which takes them off the image and changes nothing else. `Storage` keeps the four connections (24 bytes each: a vptr and two fields), and `init.cpp` declares one more global per server;
- share one set of buffers among the five resource servers instead of one per server. The root runs one `on_signals()` at a time (design D16), and `ResourceServer::handle()` needs its buffers only until it returns, so this is safe for the resource servers; it saves 4 x 4 KiB of memory as well as the file bytes, but it changes who owns the buffers, which is a design decision for the user;
- accept the cost: about 22 KiB of init's file, which the BootFS carries.

**Reference**
None, this project's own design. `docs/repairs/261004/init.md` item 9 for the Launcher's pool, the same mechanism.

**Acceptance**
`llvm-nm -S -C` on `init` lists the five `*_RESOURCE_STORAGE` and `ROOT_JOB_STORAGE` symbols as type `b` or no longer lists their buffers in `.data`; `llvm-readelf -l` on `init` shows the RW `LOAD` segment's file size back near 0x7120; and `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes both images with `routing_checks: passed`.
