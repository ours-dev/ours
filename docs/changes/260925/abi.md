# ABI 描述与生成器改动记录（2026-09-25）

## 生成器挪到 `build/abi`（2026-09-25）

**文件**
- `build/syscall/gen_syscalls.py` → `build/abi/gen_syscalls.py`（banner 里的路径随之改）
- `build/syscall/verify_vdso_abi.py` → `build/abi/verify_vdso_abi.py`
- `system/public/BUILD.gn`、`kernel/lib/userabi/vdso/BUILD.gn` — `script = ` 指向新路径
- `system/public/ours/syscalls.gni`、`docs/design/260923/userspace-interface.md` — 注释与设计里的路径

**症状**
无，纯移动。文件系统计划（`docs/design/260924/userspace-filesystem.md` §3.2）要让协议描述与系统调用描述共用同一个生成器前端，
而协议生成器放在 `build/syscall` 下名不副实。

**原因**
目录名描述的应当是它产出什么（ABI 描述的生成），而不是它的第一个使用者。

**改法**
改前：`script = "//build/syscall/gen_syscalls.py"`、`script = "//build/syscall/verify_vdso_abi.py"`。

改后：`script = "//build/abi/gen_syscalls.py"`、`script = "//build/abi/verify_vdso_abi.py"`。`git mv` 保留两个文件的历史，
内容只改 banner 那一行。

**参照**
无对应物。Fuchsia 的同类工具（fidlc、zither）各自放在 `tools/fidl` 下，按产物而不是按使用者命名。

**验证**
- 移动前把五个生成文件与 `libours.ifs` 存一份，移动后重新生成逐字节比对：五个文件只有第一行 banner 里的路径不同，
  `libours.ifs` 完全相同。
- 两个镜像照常启动：dev `init: shell closed its channel`，dbg_1 `probes done (11 killed, 0 survived)`。

**故意没做**
历史记录（`docs/changes/260924/*`、`.agents/tasks/syscall-boundary/PLAN.md`）里的旧路径不改：它们记录的是当时的样子。
