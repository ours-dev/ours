# RESULT（phys-oop-redesign）

日期：2026-09-06。状态：**已实现（工作区，未提交）**，全部验证通过。

## 交付内容

按 [COMMIT-PLAN.md](COMMIT-PLAN.md) 的 6 个提交边界，全部改动已落在工作区
（**未做任何 git add/commit**——前置条件里在途改动的处理方式仍待用户决定）：

1. `kernel/phys/detail/numeric.hpp`（`checked_to_usize`/`page_count`）+
   `detail/boot_alloc.hpp`（`allocate_top_down`，全部 inline，phys 内两份
   `to_usize` 重复与两份 top-down 分配器收敛）。
2. `BootOptions` 类化：私有成员（字段顺序不变）+ 内联访问器 +
   `PHYS_EXPORT apply()`；`parse_bool`/`parse_name` 留在 .cpp 匿名命名空间；
   physload/module_loader/physboot/smoke 全部消费者更新；
   `module_loader.cpp:119` 的 log 缺参 bug 顺手修复。
3. `MemoryProvisioner`（新头 `memory_provisioner.hpp`，注入 `BootMem&`）：
   `detect`/`reserve_boot_image`/`reserve_package`/`install` 四步；
   `register_range` 留 TU-local 自由函数供 WEAK `arch_detect_memory` 默认体
   共用；回退语义 = "容器无 memory item 才回退"（逐行对照旧代码）；
   `arch_init_memory` 保持强符号；`init_memory` 及其声明删除，stale 注释
   （`init_pmm.cpp`、`boot_package_input.hpp`、`init.hpp`）同提交更新。
4. `Aspace::map_usable_ram(PhysAddr)`（无默认实参），x86 调用点更新。
5. `bootshim::LegacyMemoryTranslator`（全 inline）：`convert()` 返回
   `Conversion`、`ranges()` 只暴露已转换前缀、二次 convert 重置；
   shim.cpp 与 smoke 更新，smoke 新增前缀不变量与重置语义两条断言。
6. ELF TU 内对象化：`SymbolicRelocationPass`（模板类，按值持
   MappedMemory，`bind()` 消掉 6 参数 × 2 调用点的穿参）+
   `SegmentMapper`（namespace 作用域，`ElfImage` friend——`LoadPlan`
   保持私有；relocate-before-map 契约注释上移到 elf_image.hpp）。
   TU 归属未动，boot-shim 单链接成立。

按评审砍掉：`RelativeRelocationPass`（单调用点）、join_path/own_build_id/
parse_string/parse_acpi 成员化（无状态单调用点）。

## 验证结果

- `ninja -C out/bringup.x86`：全绿（physload、physboot 模块
  SOLINK、x86 boot-shim 全链）。
- host smoke（bringup 的 host 图 + build-id-note 手工编译）：
  - `boot-options-smoke` PASS（已改写为 `apply()`/访问器形式，
    apply 叠加断言原样保留）
  - `legacy-memory-smoke` PASS（新增 `ranges().size()==converted` 与
    重置语义断言）
  - multiboot `view-smoke` / `memory-map-smoke` PASS
  - `build-id-note-smoke` PASS（该目标按 root BUILD.gn 注释被刻意排除在
    构建图外，用 host 编译器按相同 include 环境手工编译运行）
- clang-format：全部 22 个触及文件已 `-i` 格式化。
- clang-tidy：触及文件本体的行无未解决新诊断；`legacy_memory.hpp` 新成员
  已改为 `m_` 前缀。其余报告均为既有头文件诊断（x86_pagings、obi、
  multiboot 等的命名/[[nodiscard]]），非本次引入。
- **qemu 端到端 PASS**（`scripts/qemu-boot.sh`，GRUB/Multiboot2）：
  boot-shim 定位 physload → physload 内存发现/恒等映射（`Map identically`
  行即 `map_usable_ram`）→ physboot 模块加载绑定成功（证明导出表随
  `BootOptions::apply` mangled 名正确重生成、跨镜像内联访问器工作）→
  "handoff: jumping to kernel" → 内核完成 HPET/APIC 初始化、userboot
  进入 ring 3。

## 已知无关问题（本次不处理）

- `physical-memory-smoke` 在 out 图的默认（freestanding）工具链下因缺
  prelude 编不过——与 root BUILD.gn 注释里 `//kernel/phys:tests` 的排除
  同类，属既有配置问题；本次 diff 未触及 bootmem。

## 实施记录（2026-09-07 提交执行）

提交序列（分支 `baseline/20260824`，每个提交均过 构建 + 双 smoke + qemu
handoff 验证）：

- `0e5e10d` refactor: complete the Status rename and land the scheduler
  atomic baseline —— 在途重命名波 + scheduler 原子迁移（**HEAD 本身编不过**：
  vm_area.cpp 已用 InvArgs 而 status.hpp 未定义、scheduler.cpp 对
  ustl::sync::Atomic 调 std::memory_order——两块在途改动是 HEAD 的编译前置，
  故合并为一个基线提交）。
- `9679033` refactor(phys): share the numeric helpers and the top-down boot
  allocator
- `18bcf0a` refactor(phys): turn BootOptions into a class with an apply()
  member
- `7e376d5` refactor(phys): fold the stage-3 memory chain into
  MemoryProvisioner
- `a62c993` refactor(phys): make the RAM identity map an Aspace member
- `4fec797` refactor(bootshim): give the legacy memory translation a
  stateful object
- `323ca9b` refactor(phys): name the elf loader's bind pass and segment
  mapper

### 提交手术方法

重构与在途改动在同批文件内交织，采用"中间态重建"拆分：每个跨提交文件按
(HEAD+重命名) → 逐提交编辑 重建中间内容，`git hash-object + update-index`
精确暂存；`git checkout-index -a -f` 把工作区刷成暂存态后构建 + qemu 验证
（不用 stash——曾试过一次，pop 在重建文件上必然冲突，备份分支
`backup/phys-oop-wip` 兜底后改为确定性方案）。最终完整性校验：
`git diff backup/phys-oop-wip` 在已跟踪内容上为空，手术零丢失。

### 仍在工作区（用户自己的 WIP，未动）

syscall 相关：`kernel/lib/syscall/table.cpp`、`kernel/arch/x86/kernel/syscall.S`、
`kernel/arch/x86/BUILD.gn`、`kernel/lib/arch/BUILD.gn` 修改 +
3 个未跟踪头文件（`arch/x86/syscall.hpp` 等），以及 `scheduler_pi.cpp` 的
extern-template 清理——归用户自己提交。
