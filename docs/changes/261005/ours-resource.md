# ours-resource changes

## Programs get resources through /svc, with one client for every caller (2026-10-05)

**Files**
- `system/lib/ours.resource/include/ours_resource/client.hpp`, `src/client.cpp`, `BUILD.gn` — new `:client`, user toolchain only: `ours_resource::get(protocol)` connects to `/svc/<name>` and sends one Get; `get(connection, protocol)` does the Get on an open connection
- `system/driver/pc-ps2/pc_ps2.cpp`, `examples/shell/shell.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp` — their own copies of the Get client give way to it
- `examples/profile/profile.cpp`, `BUILD.gn` — the Sampling resource comes from `/svc/ours.resource.Sampling`, not a startup handle
- `examples/shell/shell_tests.cpp` — `check_profile()` starts profile with `fsio::spawn()` on dev, and is skipped on dbg_1, which has no namespace
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn` — profile moves from dbg_1's BootFS to dev's
- `system/service/init/test/routing_checks/routing_checks.cpp` — the Sampling check claims no session
- `docs/changes/261005/init.md`, `.agents/tasks/prof-sampler-object/DESIGN.md`, `RESULT.md`, `.agents/skills/ours-kernel-design/references/boot.md` — say so

**Symptom**
profile took the Sampling resource as a startup handle in a numbered slot, which only dbg_1's shell could hand it, so nothing on dev could profile. The client side of an ours.resource Get, about eighty lines that handle the reply, the epitaph and a closed connection, was written out three times, in pc-ps2, the shell and routing_checks.

**Cause**
Resources reached programs the way bring-up handed them out, through processargs, before the root served them as protocols.

**Change**
One client, the copy pc-ps2 had, in the library beside the codec, built only for user programs since it connects through fsio. Every caller asks by protocol, and the path follows from the name:

```cpp
auto const fetched = ours_resource::get(ours_resource::SAMPLING_PROTOCOL);
```

profile, started by the shell through the root's Launcher, inherits the shell's namespace, `/svc` included, and the root routes `ours.resource.Sampling` to the shell (previous entry in `docs/changes/261005/init.md`). The check moved to dev with it.

Booting it found a race between two components started side by side: routing_checks' Sampling check created and closed a session, and the shell's profile, which holds the one session for a few tenths of a second, could find it taken, or take it first and make routing_checks fail (it did, once: `AlreadyExists`). routing_checks now claims nothing: it hands `sampler_create` a config with `reserved` set, and since the call checks the resource before the config and the config before the session, `InvArgs` says the resource passed with no session touched.

**Reference**
Fuchsia's cpu_profiler connects to `fuchsia.kernel.SamplingResource`, which component_manager serves; components get their capabilities through their namespace, not processargs slots.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (33 passed, 3 skipped, `shell: profile checks skipped (no namespace)`, `shell: sampler checks passed`), rel PASS (31 passed, 9 skipped, 6/6 required). On dev, `profile: 40 kernel samples, 8 user samples, 0 dropped`, `shell: profile checks passed`, `routing_checks: passed`, and `pc-ps2: ready`, which takes its Irq and Ioport resources through the shared client.

**Deliberately not done**
The shell's own sampler checks on dbg_1 keep the resource userboot hands it: that image has no root to serve it.

**How it was found**
The user asked that programs reach capabilities without knowing about startup handles, and chose to serve the resource through `/svc` and share the client.

**The user's prediction**
None.

**Three questions you should be able to answer**
- How does profile, started by the shell, come to have `/svc/ours.resource.Sampling` in its namespace?
- Why did routing_checks and the shell's profile check collide, and how does `reserved = 1` avoid it?
- Why is the client built only for user programs while the codec builds on the host too?
