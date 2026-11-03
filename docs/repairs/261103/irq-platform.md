# irq-platform outstanding repairs

## 1. The route service's mode can disagree with the programmed pin when two ISA numbers share a GSI

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
