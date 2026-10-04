# obi 待修项（2026-09-25）

按严重程度排。第 2 条是一项独立的工作，涉及 phys、打包器和密钥管理，可以单独立项。

## 1. 镜像从不写校验值，启动时也就不校验

**现象**：OBI 头和每个条目都留了 32 字节的 digest 字段（`kernel/lib/obi/include/obi/package_format.hpp:90、107`），启动时的解析器在头部带
`ContainerFlags::Crc32` 时会逐项校验 CRC32（`kernel/lib/obi/include/obi/container_view.hpp:223-239`）。但打包器调用的是
`builder.build(/*crc=*/false)`（`kernel/lib/obi/cli/pack.cpp:422`，`cli/modify.cpp:112、169` 同样），所以镜像里从来没有校验值。
镜像在盘上或传输中坏了几个字节，phys 照常装载。

**改法**：`pack.cpp` 与 `modify.cpp` 改成 `build(true)`。校验代码与宿主测试（`kernel/lib/obi/test/smoke.cpp:167` 的逐位翻转测试）已经在了。
对照：ZBI 的 `ZBI_FLAGS_CRC32`。

**注意**：CRC 只防意外损坏，不防有意篡改（改完内容顺手重算 CRC 即可），后者见第 2 条。

**验收**：两个镜像照常启动；另写一个脚本（Python）把镜像里内核条目的一个字节翻转后再启动，phys 应当拒绝这个容器并报错，而不是继续装载。

## 2. 没有防篡改：条目没有签名，也没有人验

**现象**：谁能改镜像文件，谁就能换掉内核、userboot 或任何用户程序，启动链上没有任何一环会发现。

**原因**：只有校验值（第 1 条），没有签名；也没有谁在装载之前验证来源。

**改法**：

- 构建时生成一对 Ed25519 密钥。私钥只留在构建机上，永不进镜像；公钥编进 physload。
- 每个条目的 digest 字段写 SHA-256（字段本来就是 32 字节），容器头再单独签一次名。
- physload 在装载任何条目之前验签，失败就拒绝启动。
- 信任链的起点放在 UEFI Secure Boot：由固件验 bootshim/physload，physload 再验 OBI。

**不要做**：内核启动后自己校验自己。能改内核的人也能改掉校验，所以必须由上一级来验。

**参照**：Fuchsia 的 ZBI 自带 CRC32，防篡改交给 bootloader 的 verified boot（AVB/vbmeta）。这一点未在本地 checkout 核对。

**验收**：改动任一条目的一个字节，或换上另一把私钥签出的镜像，physload 都拒绝启动；正常构建的两个镜像照常启动。

## 4. 合并多个 kernel-storage 条目时，同名文件不报错，两份都进 BootFS

**现象**：一个镜像的多个 `kernel_package()`（镜像自己的与 physboot 的）被打进同一个 BootFS。2026-09-25 给 `kernel_package()`
加用户可见名单时，两个包各生成了一份 `config/user_bootfs`，结果内核包里并列出现两个同名条目（25 字节与 0 字节），打包器一声不吭。
内核按名字查文件时拿到的是第一个，哪一个在前取决于打包顺序。

**原因**：分发清单层（`build/dist`）只在同一个清单内检查"同一目标路径、不同来源"，跨多个 obi_input 合并时没有再查一遍。

**改法**：打包器合并同一类 BootFS 条目时检查目标路径唯一，重名即报错（与 `expand_and_find_conflicting_dist_entries.py`
的判据一致）。

**验收**：人为让两个 `kernel_package()` 产出同名文件，构建失败并指出两个来源；两个镜像照常构建、启动。
