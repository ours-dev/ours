# macro-fs: running progress

**Date**: 2026-10-30

- 2026-10-30 Step 1 landed: ioservice remote entries and Open remainder forwarding. Host tests 148 pass (9 new); `boot_check.py` dbg PASS, rel PASS. Change record `docs/changes/261030/ioservice.md`.
- 2026-10-30 Step 2 landed: directory capabilities from components (compiler + init routing, `docs/changes/261030/component-framework.md`), then the fshost component with /boot mounted and `vfs` routed to shell and routing_checks (`docs/changes/261030/macro-fs.md`). A read of /vfs/boot/bin/shell crosses two servers on dev; both images PASS. Two bugs found on target: the trailing slash in init's forwarded Open, and shell.cm missing the `vfs` use.

