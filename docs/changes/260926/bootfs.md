# bootfs 改动记录（2026-09-26）

## bootfs 头文件从 `kernel/lib` 挪到 `system/lib`（2026-09-26）

**文件**
- `kernel/lib/bootfs/` → `system/lib/bootfs/` — `BUILD.gn`、`.clang-format`、`.gitignore`、`include/bootfs/{builder,format,view}.hpp`，内容一字不改
- `kernel/lib/obi/BUILD.gn`、`kernel/lib/obi/cli/BUILD.gn`、`kernel/lib/userabi/BUILD.gn`、`kernel/phys/BUILD.gn` — 依赖改指向新位置
- `kernel/lib/obi/test/smoke.cpp` — 注释里的路径

**症状**
无，纯搬移。设计步骤 K14 的第 ② 个提交：下一个提交里 init 和 shell 要读 BootFS 的格式（`bootfs::Header`、`bootfs::Entry`），
而这些定义在 `kernel/lib/bootfs`。

**原因**
本仓库的分层是 `system/lib` 放通用框架、`kernel/lib` 放内核的特化（`concurrent` 与 `kconcurrent`、`libc` 都是这样）。
BootFS 的格式本来就不属于内核：phys 解包内核包、内核给用户态重建 BootFS、宿主上的打包工具（obi cli）、以及接下来的 init 和 shell，
读写的是同一种格式。让用户程序依赖 `kernel/lib` 是反向的依赖边。设计把这个搬移放在 U3（连同新增 `Tree`）；主人定的规矩是依赖先行，
所以把搬移部分提前到这里，`Tree` 仍留在 U3。

**改法**
改前：`"//kernel/lib/bootfs:headers"`、`"//kernel/lib/bootfs"`、`"//kernel/lib/bootfs:bootfs"`。

改后：`"//system/lib/bootfs:headers"`、`"//system/lib/bootfs"`、`"//system/lib/bootfs:bootfs"`。头文件的 include 写法（`<bootfs/format.hpp>`）不变，
因为 include 路径由 `cpp_interface("headers")` 按目标所在目录给出。

**参照**
Zircon 的 bootfs 格式定义在 `sdk/lib/zbi-format/include/lib/zbi-format/internal/bootfs.h`，读取库是
`src/lib/zbitl/include/lib/zbitl/items/bootfs.h`，都不在内核目录下，内核与用户态共用；这里同理放到 `system/lib`。

**验证**
- 构建通过，包括宿主工具与测试：obi 的 `container-view-test`（4 个）、`package-builder-test`（5 个）通过，`boot-options-smoke`、`cli-smoke`、
  `obi-smoke`、`lz4-wrapper-smoke` 退出码 0。
- 两个镜像启动：内核照旧从内核包重建出用户 BootFS（`userabi: bootfs vmo 40960 bytes, 4 files, magic 0x42545346`）；dev
  `init: shell closed its channel`，dbg_1 `shell: probes done (11 killed, 0 survived)`、`control probe none answered "unknown"`，
  两边 `512 channels made before the quota`。

**故意没做**
- `builder.hpp` 用 `std::vector`/`std::string`，只适合宿主工具；把它和 format/view 分成两个目标、给这个库补上 `src/` 与 `test/`，
  留给 U3，那时一起加 `Tree` 和宿主单测。
- 设计与旧改动记录里提到 `kernel/lib/bootfs` 的地方不改：改动记录只增不改，设计的 U3 行已注明搬移提前。
