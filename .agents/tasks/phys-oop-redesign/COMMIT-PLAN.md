# 提交拆分方案（phys-oop-redesign，专家评审修订版）

状态：**设计经 4 位专家（启动链架构 / C++ API / 链接-ABI-构建 / 验证-YAGNI）评审修订，
待用户确认后执行**。设计见同目录 [DESIGN.md](DESIGN.md)。

## 前置条件（执行前必须处理）

分支 `baseline/20260824` 的工作区在本次要动的同一批文件上
（`kernel/phys/*`、`kernel/lib/obi/tests/boot_options_smoke.cpp`、
`kernel/arch/x86/phys/legacy_multiboot.cpp` 等）**已带有未提交的在途改动**
（专家已核实为 `Status::InvalidArgument → InvArgs` 级别的 cosmetic diff）。
重构提交会和这些改动混在同一份 diff 里。开工前二选一：

1. 用户先把在途改动按自己的节奏提交（推荐，重构提交历史才干净）；
2. 或用户确认在途改动与本重构一起落盘，由第一个重构提交一并带上
   （不推荐，提交语义会糊掉）。

在得到明确指示前，实现阶段**只改工作区、不做任何 `git add`/`commit`**；
下表提交边界记录在案，供最终暂存时拆分。

## 提交序列（每个提交保持可编译、smoke 全绿；qemu 端到端每个提交都跑）

仓库采用 conventional commit（参照 `git log`：`refactor(mm):` / `fix(x86):`），
每个提交只承载一个聚类对象，顺序即依赖顺序：

| # | 提交 | 内容 |
|---|------|------|
| 1 | `refactor(phys): share the numeric helpers and the top-down boot allocator` | 新建 `kernel/phys/detail/{numeric.hpp,boot_alloc.hpp}`（**全部 header-only inline**：`allocate_top_down` 的两个调用方分属 physload 与 physboot 两个 ELF 镜像，out-of-line 会让 physboot 产生 hidden undefined 符号、模块链接失败；`detail/` 头用引号相对包含，不进 `:headers`）；删除 phys 内两份重复的 `to_usize`；`module_loader.cpp` 的 `bootmem_allocate`、`physboot.cpp` 的 `alloc_top_down` 收敛到 `allocate_top_down`（panic 文案统一，顺手修正 #1）。无接口变化。 |
| 2 | `refactor(phys): turn BootOptions into a class with an apply() member` | `boot_options.hpp/cpp`：私有成员（**字段顺序不变**）+ 内联访问器 + `apply()` 成员（保留 PHYS_EXPORT）；`parse_bool`/`parse_name` 留在 .cpp 匿名命名空间；更新 `physload.cpp`×2、`module_loader.cpp`（含顺手修正 #3：log 缺参 bug）、`physboot.cpp`、`kernel/lib/obi/tests/boot_options_smoke.cpp`（apply 叠加断言原样保留）。 |
| 3 | `refactor(phys): fold the stage-3 memory chain into MemoryProvisioner` | 新头 `memory_provisioner.hpp`：注入 `BootMem&` 的有状态类（可 host 测试）；`register_range` 留 TU-local 自由函数（WEAK `arch_detect_memory` 默认体也要用它）；**回退语义钉死：容器完全没有 memory item 才回退，item 在但无 Ram 是 panic**；`arch_init_memory` 保持强符号不加 WEAK；`install()` 保留 null 守卫 + 无条件 arch 调用的不对称；panic/log 文案逐字保留；删 `init.hpp` 的 `init_memory` 声明；`obi_main` 改四步驱动；携带 stale 注释更新（`kernel/mm/init_pmm.cpp`、`boot_package_input.hpp`、`init.hpp`）。 |
| 4 | `refactor(phys): make the RAM identity map an Aspace member` | `create_identity_map_for_ram` → `Aspace::map_usable_ram(PhysAddr)`（去掉死默认实参；随类级 PHYS_EXPORT 进导出表，与 map/unmap 同待遇）；`kernel/arch/x86/phys/aspace.cpp` 调用点更新。 |
| 5 | `refactor(bootshim): give the legacy memory translation a stateful object` | `legacy_memory.hpp`：**全 inline** 的 `LegacyMemoryTranslator`（convert 返回 `Conversion`，不做访问器双通道；`ranges()` 只暴露已转换前缀；二次 convert 重置；find_scratch/to_obi_type 静态成员）；`shim.cpp`、`legacy_memory_smoke.cpp` 更新（补不变量与重置语义断言）；栈生命周期注释改指对象。 |
| 6 | `refactor(phys): name the elf loader's bind pass and segment mapper` | `elf_image.cpp`：TU-local `SymbolicRelocationPass`（按值持 MappedMemory、持 machine/bias/symbols/binder，`bind()` 模板成员；`apply_kernel_relative_relocations` 与谓词保持自由函数）；`elf_image_mapping.cpp`：namespace 作用域 `SegmentMapper`（`ElfImage` 加 friend 前向声明——`LoadPlan` 是 private 别名；无 image_size 成员；relocate-before-map 契约注释上移到 elf_image.hpp 的 fix_mapping 声明处）；TU 归属不变。 |

注：join_path / own_build_id / parse_string / parse_acpi 的成员化已按评审砍掉（无状态
单调用点，匿名命名空间已足够）；无提交承载它们。

## 验证

- host smoke：需先为这批目标 `gn gen` 一个 host 目录（现有 `out/mm-host-tests`
  的图里没有 boot-options-smoke / build-id-note-smoke / boot-obi-test），然后
  `ninja -C <dir>` 跑 `boot-options-smoke`、`legacy-memory-smoke`、multiboot
  两个 smoke、`physical-memory-smoke`、`build-id-note-smoke`（目标拼写以
  kernel/lib/obi/BUILD.gn、kernel/phys/BUILD.gn、kernel/phys/lib/*/BUILD.gn 为准）
- `ninja -C out/bringup.x86`（physload / physboot module / x86 boot-shim 全链；
  boot-shim 链接是 elf TU 拆分与 header-only detail 头的金丝雀）
- 端到端：`scripts/qemu-boot.sh`（OUT=out/bringup.x86）**每个提交后跑一次**，
  通过标志 = "handoff: jumping to kernel" 日志（评审修订：原方案只跑 #3/#6，
  但 shim.cpp 无 host 覆盖、#2 的跨镜像内联访问器只有真启动能验）

## 评审清单（REVIEW.md 的输入）

- BootOptions 布局与字段顺序未变（`PhysLoadHandoff` 跨镜像 ABI）
- readelf 抽查：内联访问器未进 `.phys.exports`；`BootOptions::apply` 的 mangled
  名替换旧自由函数属预期（导出表每次链接重新生成，无静态名单）
- detect() 的回退条件 = "容器无 memory item"，非 "无 Ram 区间"（diff 逐行对照 memory.cpp:63-65/86-88/110-112）
- `arch_init_memory` 未被加 WEAK；`install()` 的 null 守卫不对称原样保留
- MemoryProvisioner 的注册顺序 = 旧顺序（detect → image → package → install）
- elf 两个 TU 的链接归属未动（boot-shim 链接成功即证）；SegmentMapper 走 friend，
  匿名命名空间里没有它
- panic/log 文案 diff 审计：全系列唯一文案变化 = 顺手修正 #1（"No memory to place {what}"）
- DESIGN「顺手修正」四条全部可追溯到代码
