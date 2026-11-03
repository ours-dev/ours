# irq-platform outstanding repairs

## 1. A firmware GSI past the platform vectors panics the kernel in the ISA override loop, where the PCI loop 25 lines below answers the same condition with warn + skip

**Severity**
Medium: a legal-but-unusual ACPI Interrupt Source Override (an ISA line mapped past GSI 207) inside `platform_init_apic()` panics — the machine never boots — where the file's own stance, stated for the identical condition in the PCI-pin loop and in the comment above it, is "gets no line and is left as the firmware left it". Not reachable on q35's firmware; wrong by construction since the file's rewrite in this window.

**Symptom**
Latent, firmware-triggered: `ASSERT(gsi <= MAX_GSI, "GSI {} of ISA IRQ {} is past the platform vectors")` at `kernel/platform/pc/interrupt.cpp:180-181`, versus `if (gsi > MAX_GSI) { log::warn(...); continue; }` at `:205-208` for the same condition.

**Cause**
The ISA loop predates the platform-vectors bound and was rewritten without the PCI loop's guard. The ASSERT's place was the old world where the bound was a hardware invariant; the PCI loop states the current design.

**Change**
One line: replace the ASSERT with the PCI loop's warn + continue.

**Reference**
The file's own comment at `:199-201`: "A pin whose GSI is past the platform vectors gets no line and is left as the firmware left it."

**Acceptance**
A unit test (or the assertion's removal making the path unreachable) — the ISA loop and the PCI loop treat the same condition the same way; nothing else in the function compares.

## 2. The route service's mode can disagree with the programmed pin when two ISA numbers share a GSI

**Severity**
Medium: latent, firmware-triggered. The kernel keeps only the first ISA line on a shared GSI (`kernel/platform/pc/interrupt.cpp:174-178`, warn + not programmed), but the route service answers per-override, last override wins (`system/service/acpi-route/src/routes.cpp:43-75`). For ISA numbers a and b both mapping to GSI g: the kernel programs g in a's mode; a `Route.IsaIrq` query for b answers gsi=g with b's own (or default) mode — and `ours_acpi.hpp` documents that reply's trigger/polarity as what `interrupt_create`'s mode has to say. A client that passes it takes the pin in the wrong mode (a level line left unmasked per interrupt, or an edge line held until each ack). Today's only client reads the pin with `ModeDefault` (`pc_ps2.cpp:172`), so nothing on q35 breaks.

**Symptom**
Latent, firmware-triggered: a `Route.IsaIrq` reply for the losing ISA number carries a mode the pin was never programmed with.

**Cause**
The route service answers per-override with no GSI-collision notion; the kernel programs first-wins. The two sides resolve collisions differently.

**Change**
The route service detects the collision (two ISA numbers resolving to one GSI) and answers the winner's mode — or answers an error for the loser — matching the kernel's first-wins programming.

**Reference**
The kernel's first-wins warn at `kernel/platform/pc/interrupt.cpp:174-178`.

**Acceptance**
A route-service host test with two ISA numbers on one GSI answers the first's mode (or refuses the second); the kernel's first-wins and the service's answer agree.
