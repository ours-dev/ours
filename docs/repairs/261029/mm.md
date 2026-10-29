# mm outstanding repairs

Found while verifying step 4 of `.agents/tasks/numa-out-of-mm/PLAN.md`, which was the first time in that task a NUMA-enabled kernel was built and booted at all.

## 1. A kernel that finds no NUMA topology says nothing about it

**Severity**
Medium. Nothing is computed wrongly; the hole is that two very different outcomes are indistinguishable in the boot output, and one of them is a silent failure. It cost a wrong diagnosis once already, on 2026-10-29 — see `docs/changes/261029/mm.md`, "A NUMA kernel on four nodes was working all along".

**Symptom**
A kernel built with `mm_numa = true` and booted on a machine with no SRAT, or with an SRAT its parse cannot use, brings up one node covering all memory and prints not a single line about it. The boot looks healthy: `PmNode::bootstrap()` only fails when no node owns memory, so one node proceeds normally through `create_nodes()`, `init()` and `connect_nodes()` and dumps a perfectly ordinary `Node[0-fallback]`.

Three further cases inside the same path are equally silent: `register_nodedis()` returns `Status::Ok` with no output when the SLIT declares zero localities, `bind_node_to_cpu()` logs nothing at all, so an SRAT walk that records zero CPU bindings leaves no trace, and the two real failure messages are both the bare string `"No NUMA"` (a missing SLIT and a rejected matrix), which a reader cannot tell apart.

**Cause**
`x86_init_numa()` tests both statuses it gets and throws them away:

```cpp
if (Status::Ok == acpi_init_numa()) { return; }
if (Status::Ok == pci_init_numa()) { return; }
```

There is no log statement on any path of that function, and it includes no logging header. Upstream, three exits in `srat.cpp` return without a word: no table set, no SRAT, and `register_meminfo()` failing.

So the information exists at the moment it is lost. Whether the firmware declared nothing, or declared something the parse rejected, is known inside `acpi_init_numa()` and is discarded one frame later.

**Change**
Report the fallback rather than returning void silently: capture the status, and when neither source succeeds, log that no firmware topology was found and that one node will cover all memory. `numa.cpp` needs `<logz4/log.hpp>` added — it currently includes no logging header.

Then give each silent exit in `srat.cpp` its own line: no SRAT found, the SLIT's locality count, and the number of CPU bindings recorded. Make the two `"No NUMA"` strings name their cause.

All four sites run at `PrePmm + 1`, where `log::info` and `log::trace` demonstrably reach COM1 — the `NUMA information from ACPI-SRAT` line from the same stage arrives, so this needs no new plumbing.

Rejected: asserting instead of logging. A machine without an SRAT is an ordinary machine, not an error, and the overwhelming majority of boots are exactly that.

**Reference**
Linux's `numa_init()` in `arch/x86/mm/numa.c` falls back to `dummy_numa_init()` — one node over all memory — when any stage fails, and that function announces itself: `pr_info("Faking a node at [mem %#018Lx-%#018Lx]\n", ...)`. The point is not the wording but that the fallback is never silent.

**Acceptance**
Boot a kernel built with `mm_numa = true` on a machine with no NUMA topology, which is the default QEMU invocation:

```bash
python3 tools/set.py bringup.x86 --out out/numa.x86 --args 'mm_numa=true'
python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/numa.x86
```

The log must carry a line saying no firmware topology was found. With `NUMA=1` it must not, and must instead report four nodes, as it already does.
