# logz4 重设计 — 状态

按 PLAN.md 落地，调用点零改动。

已完成：
- 核心在 `system/lib/logz4`（栈上装配整行、单次 write、atomic logger、MPSC ring、hooks、`set_min_level`）
- `kernel/lib/logz4` 只留胶水：COM1 SerialLogger、emergency_write、TSC/CPU hooks、kshell `log`
- GN 依赖改指向；死头文件已删
- panic 转储 ring；phys `log.level=`
- hosted `logz4-test`、physload 链接、boot-options-smoke 均通过

验证：
- `./out/bringup.x86/host/obj/system/lib/logz4/logz4-test`
- `./out/bringup.x86/host/obj/kernel/lib/obi/tests/boot-options-smoke`
- `ninja -C out/bringup.x86 kernel.phys_x86.phys.dbg_0/obj/kernel/phys/physload`
