# eftl 改动记录（2026-09-19）

## PLT 重定位表不再要求 DT_RELAENT，读不出的表不再被当成没有（2026-09-19）

**文件**
- `kernel/lib/eftl/include/eftl/elf/dynamic_image.hpp` —— `relocation_table_from_tags()`：
  条目大小标签在时校验，不在时不再拒绝。
- `kernel/lib/elfload/src/binder.cpp` —— 新增 `declares()`；镜像声明了 `DT_RELA` /
  `DT_JMPREL` 但 eftl 读不出来时，`Binder::bind()` 报错拒绝，而不是跳过。

**症状**
shell 链上 `//system/lib/ou` 之后，一进 ring 3 就缺页：
```
Unhandled page fault: vector=14, error_code=0x14
  rip=0x15c6 (+0x77c00015c6)  cs=0x33
```
`rip=0x15c6` 是程序的**链接期**地址（镜像映射在 `0x200000`），也就是说跳转目标
没被重定位。debug 镜像里连 `elfload: bound ...` 都没打——绑定器一条重定位都没应用，
也没报任何错。

**原因**
两层叠在一起。

第一层是 eftl：`dynamic_plt_rela_table()` 用 `DT_RELAENT` 当 PLT 表的条目大小标签，
而 `relocation_table_from_tags()` 要求这个标签**必须存在**。但 `DT_RELAENT` 描述的是
`DT_RELA` 的条目；`DT_JMPREL` 没有自己的大小标签——`DT_PLTREL` 给出的是条目**类型**，
类型本身就决定了大小。链接器只在对象真有 `DT_RELA` 时才发 `DT_RELAENT`。这次的 shell
恰好所有重定位都是 PLT 槽位：
```
JMPREL    0x318
PLTRELSZ  48 (bytes)
PLTREL    RELA
（没有 RELA / RELASZ / RELAENT）
```
于是整张 `.rela.plt` 被读成「不存在」。

第二层是绑定器：eftl 对「没有这张表」和「有但读不出来」都回 `NONE`，而绑定器把两者
一律当成前者——`relas.is_none() && plt_relas.is_none()` 就 `return Status::Ok`。GOT
保留着链接器写进去的链接期值，第一次经它调用就跳到了未重定位的地址。**错误的「成功」
比失败危险得多**：故障发生在 ring 3、离原因很远的地方，现场什么都说明不了。

**改法**
eftl 侧，条目大小标签「在则校验、缺则按类型」：
```cpp
// 改前
auto const entry_size = unique_tag_value(entry_size_tag);
if (address.is_none() || size.is_none() || entry_size.is_none()) {
    return ustl::NONE;
}
... || entry_size.value() != sizeof(Entry)) { return ustl::NONE; }

// 改后
if (address.is_none() || size.is_none()) {
    return ustl::NONE;
}
auto const entry_size = unique_tag_value(entry_size_tag);
if (entry_size.is_some() && entry_size.value() != sizeof(Entry)) {
    return ustl::NONE;
}
```
标签存在但与类型不符仍然拒绝——现有的 `test_rejects_malformed_dynamic_relocation_metadata`
测的正是这个，不受影响。

绑定器侧，读不出来的已声明表一律拒绝：
```cpp
if (plt_relas.is_none() && declares(dynamic.value(), eftl::elf::DynTag::JmpRel)) {
    log::error("elfload: the importer declares DT_JMPREL but it cannot be read");
    return Status::InvArgs;
}
```
`declares()` 自己扫一遍动态段，因为 eftl 的 `contains_tag()` 是私有的；为这一处调用
去放宽 eftl 的公开接口不值得。

**参照**
`Linux fs/binfmt_elf.c` 不做这件事（它只负责映射，重定位交给 ld.so）；对照物是
`glibc elf/do-rel.h` 的 `elf_dynamic_do_Rel()`：DT_JMPREL 的条目大小取自
`DT_PLTREL` 所指类型（`ElfW(Rela)` 或 `ElfW(Rel)`），不查 `DT_RELAENT`。
`Zircon elfldltl::RelocationInfo` 同样由 `DT_PLTREL` 决定 `jmprel` 的解释方式。

**验证**
- 构建通过；clang-format 干净。
- QEMU 实测：**在 shell 仍带着 `.rela.plt`（两条 `R_X86_64_JUMP_SLOT`）的状态下**启动
  `kernel.dev.dbg_1.obi`，修之前缺页于 `rip=0x15c6`，修之后：
  ```
  [debug] elfload: bound 'ours_debug_write' to A012F0
  [debug] elfload: bound 'ours_command_list' to A01300
  ours shell: the kernel's command registry, seen from ring 3
  --- end ---
  ```
  也就是说 JUMP_SLOT 这条路径是实测过的，不只是「代码上接受这个类型」。
- eftl 的宿主测试**未运行**：`kernel/lib/eftl/test/` 下的目标没有挂进任何构建图
  （`gn ls` 在两个输出目录里都找不到它们），这是既有问题，记在 repairs。

**故意没做**
- 没给 eftl 加「只有 DT_JMPREL 没有 DT_RELA」的新测试用例——测试目标目前构建不到，
  加了也不会跑。先把测试接进构建图，再补这条。
- 没把 `contains_tag()` 公开。
