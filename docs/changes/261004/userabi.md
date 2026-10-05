# userabi changes

## Title: the kernel mints the SAMPLING system resource for the first process, and userboot hands it on (2026-10-04)

**Files**
- `system/public/ours/syscall/resource.hpp` — `RSRC_SYSTEM_SAMPLING_BASE = 15`; `RSRC_SYSTEM_COUNT` from 3 to 16; bases 3 to 14 said to be unused
- `kernel/lib/userabi/src/userboot.cpp` — `write_bootstrap()` mints the slot beside VMEX and DEBUG; the handle arrays grow from 7 to 8; the message is sized by `sizeof(infos)`; the startup-handle list in its comment gains the SystemResource entries it never had
- `kernel/lib/userabi/userboot/userboot.cpp` — takes the handle, says so if it is missing, and hands it to the next program with the others; arrays from 6 to 7, the message sized the same way
- `.agents/tasks/prof-sampler-object/PLAN.md`, `DESIGN.md` — step 3 as built; the open question on how the driving program gets the resource narrowed to the hop after userboot

**Symptom**
None: a new capability. Step 3 of `.agents/tasks/prof-sampler-object`: `sampler_create` (step 4) needs a System resource at the SAMPLING slot to check, and nothing minted one.

**Cause**
A System resource of size 1 at base N is the capability slot N names, and only the kernel creates one (`ResourceDispatcher::create()` is reachable from `make_system_resource_handle()` alone). So a new privileged operation needs two things: a slot number in the public ABI, and a place in the kernel that mints it and hands it to whoever is trusted to give it out. That place is `write_bootstrap()`, which builds the first process's processargs, because the first process is where the system's authority starts: everything else gets a capability because something holding it chose to pass it on.

Three choices were made, each for a reason worth knowing:

- **The slot number is 15, not 3.** Slot numbers are ABI: a program built today must mean the same slot as the kernel it runs on in a year. The slot table is a fixed list, with numbers assigned to capabilities before this kernel has them (base 0, the hypervisor's, is already left unused). Taking the next free number would make 3 mean SAMPLING here and something else everywhere the list came from. `RSRC_SYSTEM_COUNT` becomes 16, so `ResourceDispatcher::create()` would also accept 3 to 14; nothing asks for those, since only the kernel mints.
- **Always minted, with no boot option.** DEBUG is minted only when the boot options ask for debug input, because holding it lets a program read the console. SAMPLING costs nothing until someone holding it starts a session (step 1 made the profiler free while idle), so a boot-time gate adds nothing the capability does not already give. That is design decision D4, "resource only for now".
- **userboot hands it on.** `start_next()` promises the next program exactly what the kernel handed userboot, so that it starts as if the kernel had started it. A handle userboot took and dropped would break that promise silently. So it is passed on like DEBUG: to the shell on `dev.dbg_1`, to init on `dev`. Neither takes it yet. An unclaimed startup handle stays in the process's table, and who serves it onward (init's `/svc`, as it serves Debug) is decided with the program that uses it, in step 5.

**Change**
The slot:

```cpp
// Bases 3 to 14 belong to capabilities this kernel does not have yet, and are
// left unused, as base 0 is.
inline constexpr u64 RSRC_SYSTEM_SAMPLING_BASE = 15;
inline constexpr u64 RSRC_SYSTEM_COUNT = 16;   // was 3
```

The kernel mints it after DEBUG, and a failure fails the boot, as a failure to mint VMEX does:

```cpp
handles[count] = make_system_resource_handle(RSRC_SYSTEM_SAMPLING_BASE, "sampling");
if (!handles[count]) {
    return Status::NoResource;
}
infos[count++] = handle_info(HandleType::SystemResource, u16(RSRC_SYSTEM_SAMPLING_BASE));
```

userboot reports a missing one but carries on, because nothing it does needs the profiler:

```cpp
if (sampling == ours::Uandle::Invalid) {
    auto _ = ort::debug_line("userboot: started without the sampling resource");
}
```

Both message buffers were sized by a literal handle count (`7 * sizeof(u32)` in the kernel, `6 * sizeof(u32)` in userboot) kept by hand next to an array of the same length. Adding a handle means changing both numbers, and missing the second one makes `encode_process_args()` fail for want of room on exactly the boot that has every optional handle. Both are now `sizeof(infos)`, so the message grows with the array.

Rejected: handing userboot a single System resource covering every slot, from which a later program carves out per-slot ones. This kernel mints one handle per slot (VMEX's design, D3), and nothing here can carve a resource yet.

**Reference**
Zircon `ZX_RSRC_SYSTEM_SAMPLING_BASE` (15) and `ZX_RSRC_SYSTEM_COUNT` (16) in `zircon/system/public/zircon/syscalls/resource.h`, checked in the local Fuchsia checkout at `51f05104c13`; `zx_sampler_create()` checks it with `validate_ranged_resource(rsrc, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_SAMPLING_BASE, 1)` (`zircon/kernel/lib/syscalls/sampler.rs`). Zircon's kernel hands userboot one System resource over the whole range (`get_resource_handle(ZX_RSRC_KIND_SYSTEM)` in `zircon/kernel/lib/userabi/userboot.cc`), and component_manager serves per-slot ones such as `fuchsia.kernel.SamplingResource`. Here the kernel mints the slots one by one, as it does VMEX.

**Verification**
clang-format clean on the changed lines; clang-tidy reports nothing in the two userboot files. `boot_check.py --out out/sampler`: build ok, dbg PASS (29 passed, 2 skipped), rel PASS (26 passed, 7 skipped, 2/2 required), and neither log has `started without the sampling resource`. The userboot binary contains that string, so the check is compiled in. Negative control, not committed: with the four minting lines removed from `write_bootstrap()`, dbg prints `userboot: started without the sampling resource` before `userboot: starting bin/shell`, and still passes.

Not verified: that the handle names slot 15 rather than some other slot. Nothing in user space can ask a resource its base yet; `sampler_create` in step 4 is the first caller to check it.

**Deliberately not done**
- init and the shell do not take the handle, and init does not serve it in `/svc`: that is how the driving program gets it, decided in step 5.
- No resource-info syscall to read a resource's kind and base.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the SAMPLING slot 15 rather than 3, and what would go wrong for a program if slot numbers were handed out in order?
- Why does a failure to mint SAMPLING fail the boot in the kernel, while its absence in userboot only prints a line?
- What did sizing the processargs buffer by a literal count risk, and on which boot would the mistake have shown?
