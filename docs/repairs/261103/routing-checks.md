# routing-checks outstanding repairs

## 1. `check_gpt` can fail where the block check skips, when there is no disk

**Severity**
Low: an intermittent failure of the rel boot check, not a defect of the system under test, but it makes `verify` fail without a cause in the change being verified.

**Symptom**
One rel boot in seven, on a build that otherwise booted six times in a row, ended `rel FAIL  2 FAILED line(s) ... 6/7 required`. With no disk attached, the log read, in order: `virtio-blk: no virtio block device on bus 0`, `init: virtio-blk exited with 0`, `btreefs: no partition table: PeerClosed`, `routing_checks: no block device, skipped`, then `routing_checks: Gpt.GetInfo = PeerClosed (expected Ok) FAILED` and `routing_checks: no disk directory (PeerClosed), skipped`. The run in question was the first of a `boot_check.py` that also runs the host tests, so the machine was busy; the six that passed ran with nothing else going.

**Cause**
Not diagnosed. The block check (`routing_checks.cpp:1874`, `:1882`) skips when there is no block device, and `check_gpt()` (`routing_checks.cpp:2482`) expects `Status::Ok` from `Gpt.GetInfo` at `:2493` and counts `PeerClosed` as a failure. The two seem to disagree about what "no disk" looks like when the driver has already exited, and which one a boot meets depends on timing.

**Change**
Find out what answers `Gpt.GetInfo` when the driver has exited, and make `check_gpt()` skip exactly where the block check does. Do not widen the expectation to accept `PeerClosed` everywhere: with a disk attached it would hide a real failure.

**Reference**
None, this project's own design.

**Acceptance**
`boot_check.py --no-build --only rel` repeated twenty times, with the host tests running alongside on some of them, never fails.

## 2. routing_checks has no room left for another directory use

**Severity**
Low: nothing fails today, and the next directory use added to its manifest fails the build or the boot until three constants are raised together.

**Symptom**
Latent, cannot be triggered today. After `/slow` (step 9 of the directory-capability design), `routing_checks.cm` has 15 directory uses, which is `init::MAX_DIRECTORY_USES` (`system/service/init/pools.hpp`), and its namespace has 16 bindings (the 15 and `/svc`), which is `fsio::MAX_BINDINGS` (`system/lib/fsio/include/fsio/namespace.hpp`). A sixteenth use is past both tables: `Namespace::bind()` answers NoResource for the seventeenth binding (`namespace.hpp`, `MAX_BINDINGS`), and the pool arithmetic of `pools.hpp` no longer describes the component. `main()` checks the count (`bound != 16`) and says so.

**Cause**
One component holds every on-target directory case, and each provider's directory is a use of its own (`/files`, `/files-sub`, `/files-denied`, `/files-wide`, `/files-file`, `/ghost`, `/slow`, beside `/boot`, `/boot-r`, `/bin`, `/denied`, `/vfs`, `/vfs-all`, `/disk`, `/dev`). The three limits are separate numbers that must move together: the uses (`MAX_DIRECTORY_USES`), the bindings (`MAX_BINDINGS`, a use and `/svc`), and the handle charge per component (`HANDLES_PER_COMPONENT = 8 + 2 * MAX_DIRECTORY_USES`, whose `static_assert`s against the process quota would then be re-checked).

**Change**
Raise the three together when the next directory case needs a use, and re-check the `static_assert`s in `pools.hpp`; or give a second test component the directory cases of a provider that is not `files`. Not done now: step 9 fits exactly.

**Reference**
None, this project's own design: D27 of `docs/design/261004/component-framework.md` (fixed pools).

**Acceptance**
A use added to `routing_checks.cm` boots with `routing_checks: passed`, and `fsio::installed_namespace().size()` in `main()` matches the new count.
