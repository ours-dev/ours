# object outstanding repairs

## 1. MessagePacket zeroes each message's bytes before the user copy overwrites them

**Severity**
Low: up to 4 KiB of extra stores per channel write, through the byte-loop memset (libc item 1); the bytes are never read before they are written.

**Symptom**
Since 6d805b4d the bytes are a `ktl::Vec<u8>`, whose allocator value-initializes; `kmalloc` used to hand the buffer over unzeroed. The zeroing does protect against a future path that publishes bytes it never wrote: the buffer shares kmalloc caches with every other kernel object, so such a bug would leak them.

**Cause**
Vec's allocator contract: every slot is a live, value-initialized object.

**Change**
Options, for the user: an opt-in `Vec::resize_and_overwrite(n, fill)` with an allocator path that does not zero (debug builds poison the buffer first); or keep the zeroing and make it cheap (libc item 1); or leave it. Measure first, as the user decides: a one-thread user program writing and reading one channel in a loop at 0, 64, 256, 1024 and 4096 bytes, with 0 and 1 handles, timed in batches of about 1000 round trips with rdtsc (the guest TSC moves in 1 us steps under TCG), five boots per build, alternating builds, comparing the build before 6d805b4d, HEAD, and HEAD with the payload left unzeroed (a measurement-only patch). The 0-byte cell is the control.

**Reference**
Zircon leaves payload pages uninitialized on purpose (`zircon/kernel/object/buffer_chain.rs`, `mbuf.rs`), but its IPC pages come from their own page cache, so a missed write can only show an old message.

**Acceptance**
The measurement is recorded and the user's choice is built, or the item is closed as not visible.

## 2. The handle slots moved up one kmalloc size class when the count is a power of two

**Severity**
Low: memory, not time; at most 8 bytes per handle in flight.

**Symptom**
`Vec<ustl::Box<Handle>>` allocates with `new T[n]()`, which puts a one-word count in front of objects with destructors: 64 handles take 520 bytes, a kmalloc-1024 object instead of kmalloc-512.

**Cause**
The array cookie of `new[]` for non-trivially destructible types.

**Change**
Leave it unless memory matters; a raw-storage container (`StaticVec`-style) would not carry the word.

**Reference**
None.

**Acceptance**
None needed while memory is not a concern.

## 3. A message takes three allocations and three frees, each through shared kmalloc locks

**Severity**
Low until measured: for small messages this is likely the largest cost of a write.

**Symptom**
Header, bytes and handle slots are separate allocations; each allocate and free takes the CPU cache's local lock and a spinlock shared by all CPUs, with interrupts saved.

**Cause**
The class keeps each part in kmalloc to stay off vmalloc.

**Change**
After measurement only: one block for header, handles and payload when they fit 4 KiB (Zircon's layout), tied to the question of 64 KiB messages and buffer chains. A strategy decision for the user.

**Reference**
Zircon `message_packet.rs` `create_common`: one `BufferChain::alloc`, header, handles and the first payload bytes in one page.

**Acceptance**
The measurement shows allocation's share of a write.

## 4. No test runs the undo in VmaDispatcher::allocate() for a sub-region whose dispatcher cannot be made

**Severity**
Low: a missing test. The branch is one call, `m_vma->destroy_child(*child)`, and `vmaundo.test` runs that same `destroy_child()` on an unreported sub-region through `undo_allocate()`; but nothing runs the branch itself.

**Symptom**
None visible. The branch in `VmaDispatcher::allocate()` taken when `create()` fails (`kernel/object/vma_dispatcher.cpp:344-347`) never runs in any test. A regression there, such as a return before the undo, would leave a sub-region reserved that nobody was told of, until its address space dies, and every check would still pass.

**Cause**
`create()` fails only when allocating the dispatcher fails (`new (mm::GAF_USER) Self`), and no allocation fails today: the allocator spins when memory runs out (`docs/repairs/260908/heap.md` item 3), and nothing can make one fail on purpose. So the acceptance of `docs/repairs/261002/object.md` item 3, which the branch fixed (`docs/changes/261005/object.md`, the entry on undoing by the object), could not be run, and is carried over here. Depends on that heap item, or on a way to inject an allocation failure.

**Change**
A `kernel/commands` test that builds its own address space and region, as `vmaundo.test` does, makes the dispatcher's allocation inside `allocate()` fail, and checks that the call answers OutOfMem and that its range is free again.

**Reference**
None. Zircon's `VmAddressRegionDispatcher::Allocate()` (`zircon/kernel/object/vm_address_region_dispatcher.cc:128`) has no undo between `CreateSubVmar()` and `Create()`, so there is no counterpart test.

**Acceptance**
With allocation failure injectable, an `allocate()` whose dispatcher cannot be made answers OutOfMem and leaves its range free: the same request placed again lands at the same address. The test passes on dev.dbg_1.
