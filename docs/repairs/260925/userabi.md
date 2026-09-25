# userabi 待修项（2026-09-25）

## 1. 交给用户态的 BootFS VMO 里带着内核映像

**现象**：潜伏。文件系统计划的 K12 会把整个内核包做成 BootFS VMO，交给 userboot，再由它转交给 init。内核包里有
`vmours/image.elf` 和 `vmours/code-patches`（`kernel/products/bin/BUILD.gn:19` 的 `has_patches = true`）。设计里 init 只在
建目录树时把它们藏起来（`docs/design/260924/userspace-filesystem.md` §4.1），但这只对拿 `/boot` 连接的程序有效；持有原始 VMO
的 userboot 和 init 仍能用 `vmo_read` 按偏移读出完整的内核 ELF 和补丁表。内核带 KASLR（`kernel/phys/physboot.cpp:144-148`），
映像等于把布局交了出去，只差一个基址。

**原因**：用户 BootFS 和内核包是同一个 BootFS。藏起来只是表面上看不见，字节还在那个 VMO 里。

**改法**（二选一，前者更小）：

- K12 建 BootFS VMO 时只拷用户条目，跳过内核自己的目录。目录名是启动选项 `kernel.name`，phys 已经在解析它
  （`kernel/phys/boot_options.cpp:42`、`physboot.cpp:123-128`）；vmours 自己的 `BootOptions`（`kernel/include/ours/boot/options.hpp`）
  读的是同一个 OBI 命令行条目，但还没有这一项，要补上。
- 或者把设计 D7 的长期方案提前：打包时把用户程序放进独立的 `ItemType::StorageBootFs` 条目（`kernel/lib/obi/include/obi/package_format.hpp:21-22`，
  格式里已有），内核只把它做成 VMO。对照 Fuchsia：内核包和用户 bootfs 是两类条目（`zircon/kernel/phys/kernel-package.cc:65`）。

两种做法都能去掉设计里 init 的隐藏清单 `config/boot_private`，同时修订设计 §4.1、§6.5 的"内核映像泄露"一行和 D7。

**定下的做法**（2026-09-25，主人）：黑白名单，而不是"跳过内核目录"这一条特判——内核包以后会装什么谁也说不准，
写死今天已知的那一项，明天新加的私有文件就会悄悄漏出去。

- **白名单决定放行什么，默认不放行**：由构建从 GN metadata 生成用户可见条目的名单（标记在产出文件的 target 上，
  如 `user_executable()`），随内核包打入；K12 建 BootFS VMO 时只拷名单里的条目，其余一律留在内核包里。
- **黑名单兜底**：内核自己的目录（启动选项 `kernel.name`）与补丁表，即使误入白名单也拒绝拷出，并在启动时报错——
  名单是构建产物，出错的方向必须是"少给"。
- 设计里 init 的隐藏清单 `config/boot_private` 随之取消，init 不再需要隐藏任何东西。

**不要做**：给内核映像加密。解密密钥只能和密文放在同一个镜像里；而且用户态拿到的是解密之后放进内存的那一份，加密保护不到它。

**验收**：dbg_1 里一个持有 BootFS VMO 的程序（今天是 shell）遍历 BootFS 头部和条目表，打出的文件名里没有内核目录；
按偏移读原内核条目所在的区间，读不到 ELF 魔数。
