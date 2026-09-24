# syscall 待修项（2026-09-23）

## 1. 删除临时的 `ours_process_spawn_boot`

**删除条件：** P1-C 的用户态加载器落地——即 init 能在用户态从 BootFS VMO 装载一个程序（`process_create` /
`thread_create` / `process_start`、`vmo_create` / `vmar_map`、processargs、移到 `system/lib` 的 Binder）。

**要删的东西：**
- `OURS_SYS_PROCESS_SPAWN_BOOT`、`PROCESS_BOOT_NAME_MAX`（`system/public/ours/syscall.hpp`）
- `ours_process_spawn_boot` 的声明与桩（`system/public/ours/vdso.hpp`、`kernel/lib/userabi/vdso/vdso.S`）
- `kernel/lib/syscall/src/process.cpp` 与它在 `table.cpp` 的表项
- `//kernel/lib/syscall` 对 `//kernel/lib/userabi` 的 GN 依赖——这条边是「系统调用只碰 kernel/object」规则唯一的例外
- `ou::process_spawn_boot`（`system/lib/ou/include/ou/process.hpp`、`src/process.cpp`）

**为什么现在存在：** 它是在用户态加载器之前起第二个程序的唯一办法，方案决策 D1 选了它。围栏：只接受内核包里、
内核自己装载过的文件，ring 0 不解析任何程序提供的 ELF；装载代码与内核起第一个程序的 `userabi::launch` 是同一份。
