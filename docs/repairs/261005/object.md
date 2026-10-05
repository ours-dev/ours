# object outstanding repairs

## 1. Two creates on one free line can each set or read its mode before either takes it, and the winner may run with the other's mode

**Severity**
Medium: an interrupt object can end up on a level-triggered line without the mask that keeps the line quiet until the ack, and the line then fires again after every end of interrupt until the driver has serviced its device. It needs two callers holding an IRQ resource that covers one line to create on it at the same moment, which no program does today: the only resource holder is the root on dev and the shell on dev.dbg_1. Zircon has the same window. Found by reading the code while finishing step 11 of `.agents/tasks/microkernel-interrupts/PLAN.md`, not reproduced.

**Symptom**
Latent. After the interleaving below, the object that holds the line was built for one trigger mode while the IO-APIC entry holds the other, and either way round is wrong:
- An edge object on a level entry: each interrupt is delivered with the line left unmasked, the level is still high when the end of interrupt goes out, and the CPU that takes the line's vector takes it again at once, folding each time into the pending interrupt, until the driver quiets the device and acks.
- A level object on an edge entry: the line is masked from every interrupt until the ack, and the IO-APIC ignores an edge that arrives on a masked pin, neither delivering it nor holding it pending (the 82093AA datasheet's mask bit). An interrupt raised in that window is lost, which is why D2 leaves edge lines unmasked.

**Cause**
The objects:
- `InterruptEventDispatcher::create()` (`kernel/object/interrupt_event_dispatcher.cpp`) decides the object's flags from the line's trigger mode: MaskPostwait and UnmaskPrewait for a level line, none for an edge one. With a mode in the options it sets the mode itself through `irq::configure()`; with ModeDefault it reads the mode through `irq::trigger_mode()`.
- `irq::configure()` takes the line's lock, L1, and refuses a line that has an observer, so a live line's mode never changes under its owner. `irq::trigger_mode()` takes no L1, only the IO-APIC's register lock, LI.
- `irq::attach()` takes L1 and refuses a second exclusive observer with AlreadyBound.

Each step holds L1 for itself only. Nothing holds the line from the mode step to the attach, which is what `IrqObject::configure()`'s comment already says: "it does not keep two callers' configure-then-attach sequences apart, which is theirs to serialise". `create()` does not serialise them.

The events, with creates A and B on the free line GSI 10:
1. A, ModeEdgeHigh: `irq::configure(10, Edge, ActiveHigh)` finds no observer and writes the entry: edge. A's flags are none.
2. B, ModeLevelHigh: `irq::configure(10, Level, ActiveHigh)` finds no observer either, since A has not attached yet, and writes the entry: level. B's flags are MaskPostwait and UnmaskPrewait.
3. A: `irq::attach()` succeeds and `irq::unmask()` opens the line.
4. B: `irq::attach()` answers AlreadyBound, and B's object is dropped without destroy(), as it must be.

A now owns a level entry with an edge object's flags: the first interrupt is delivered with the line unmasked, and the level re-fires it until A's driver acks. The same holds with ModeDefault in A: A reads edge in event 1, B writes level in event 2.

**Change**
Options, for the user to choose, since each is a locking decision:
- (a) Make the mode and the attach one step under L1: an `irq::attach()` that takes the mode to set, or answers the mode it found, and sets or reads it under the same L1 hold that links the observer. The object's flags then come from what the attach saw. Nothing new is locked; L1 → LI is already the order `configure()` takes. Recommended.
- (b) A mutex in `InterruptEventDispatcher::create()` around the mode step, the attach and the unmask. It serialises creates of interrupt objects only, which is what races here, but leaves the irq layer's own interface as open to the same mistake by the next caller.
- (c) Leave it as Zircon has it, and say in `interrupt_create`'s documentation that two creates on one line at once may leave the line in either mode.

**Reference**
Zircon `InterruptEventDispatcher::Create()` (`zircon/kernel/object/interrupt_event_dispatcher.cc`), which calls `get_interrupt_config()` or `configure_interrupt()` and then `RegisterInterruptHandler()`, each taking `InterruptManager`'s lock for itself (`zircon/kernel/platform/pc/interrupt_manager.rs`, `configure_interrupt()`, `get_interrupt_config()`, `register_interrupt_handler()`): the same window.

**Acceptance**
A kernel test in `kernel/commands/interrupt_line_test.cpp` that runs two creates on one free line from two CPUs many times, with opposite modes, and checks after each round that the winner's mode, read through `irq::trigger_mode()`, is the one it asked for; `interruptline.test: ok` on dev.dbg_1.

## 2. No test sees a level line masked until its ack

**Severity**
Low: a missing test. The code path is the same one the edge tests run, plus one `irq::mask()` in the handler and one `irq::unmask()` at the ack or the next wait, and it rests on that and on argument.

**Symptom**
`interruptline.test` raises its interrupts by IPI on the line's vector, which goes around the IO-APIC and its mask, and nothing reads a redirection entry's mask bit back. So no test shows that a level object masks its line when it delivers and unmasks it at the ack, nor that an edge object leaves the line alone.

**Cause**
QEMU's q35 has no device on a free level-triggered line before PCI: the firmware makes ISA 5, 9, 10 and 11 level, active high, and the only one of them a device raises is 9, the ACPI SCI. The irq layer has no call that reads a line's mask, as Zircon's has none.

**Change**
Options:
- (a) A real level source: the ACPI power management timer's overflow event. With TMR_EN set in PM1_EN, the chipset sets TMR_STS each time bit 23 of the 24-bit timer changes, about every 2.3 s at 3.58 MHz, and raises the SCI, a level line on GSI 9, until TMR_STS is written back; that is the ACPI specification's fixed-event model, and QEMU's ICH9 emulates the timer's events. A test would create on ISA 9 with ModeDefault, which reads level, set TMR_EN, and check that the handler runs once per event, not again until TMR_STS is cleared and the interrupt acked. It needs the FADT's PM1a_EVT_BLK, which the kernel does not decode yet (`kernel/lib/acpi` has no FADT), and the claim that QEMU raises the SCI this way is not checked here. Recommended, as it tests the path a real level device takes.
- (b) A read-back of a line's mask through the irq layer, used only by the test: smaller, but an interface with no other caller.

**Reference**
None for the test itself. The ACPI specification's PM1 event registers (TMR_STS, TMR_EN) and the SCI.

**Acceptance**
`interruptline.test` on dev.dbg_1 checks, on a real level interrupt or through the read-back, that a level object's line is masked from delivery to ack and an edge object's line never is.

## 3. MessagePacket zeroes each message's bytes before the user copy overwrites them

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

## 4. The handle slots moved up one kmalloc size class when the count is a power of two

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

## 5. A message takes three allocations and three frees, each through shared kmalloc locks

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

## 6. No test runs the undo in VmaDispatcher::allocate() for a sub-region whose dispatcher cannot be made

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
