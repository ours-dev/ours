# 设计:kernel.dev.obi 目标树 + phys 三级加载链(全程参数化)

状态:v3。第 1-3 步已实施(见 §8 实施记录);第 4-5 步被 phys console
重构的存量未完成部分阻塞(见 §8.3)。
范围:OBI 打包链路(build/obi + obit)、kernel/phys 拆分、x86 boot-shim、physboot 模块化。
对标:Fuchsia kernel.zbi 拓扑(IMAGE_SLOT + cmdline + STORAGE_KERNEL)。

## 已定决策

| 决策点 | 结论 |
| --- | --- |
| item 0 格式 | 完整 ELF item(type=EXEC, format=Elf),消费者是 x86 boot-shim |
| item 0 消费者 | GRUB/Multiboot2 → x86/phys shim → shim 从 OBI 取 physload 并加载 |
| build-id 配对 | 本期不做校验,机制(kernel_elf_interp)保持闲置 |
| kernel.phys.next 未命中 | 告警并回退默认 "physboot";回退也缺失才 panic |
| userboot 等槽位 | 留空不占位,只标注挂点 |
| 查找对象 | 三跳全部参数化(见 §0),任何一跳都不写死目标名 |
| 包内文件命名 | "image.elf"/"code-patches" 是 `obi::KernelPackage` 的 static constexpr,单一事实源 |

## 0. 启动链路(用户确认)

```
GRUB (Multiboot2)
  └→ x86/phys bootloader(shim,GRUB 加载的独立映像)
       读 OBI(multiboot module),按名定位加载对象(默认 "physload",参数化)
       → 取出 physload ELF → 装载 → 跳转(传 OBI 区间)
  └→ physload(OBI 内 item 0)
       解析 OBI(BootObi)→ boot options → 内存/临时地址空间
       → 定位 kernel package(KSTR)→ 解压 → BootFS
       → 按 kernel.phys.next(默认 "physboot")在 BootFS 找 module
       → 以"加载模块"方式装载(ET_DYN 重定位)→ 跳转
  └→ physboot(BootFS 内的独立动态模块)
       按参数(默认目录 "vmours")在 BootFS 定位内核镜像目录
       → image.elf 装载/relocate + code-patches 打补丁
       → 交棒内核
  └→ vmours
```

要点:

- **physload 与 physboot 是两个独立二进制**。physload 在 OBI 里(item 0),
  physboot 在 kernel package BootFS 里;physload 按加载模块的方式
  (ET_DYN + 重定位)把 physboot 装起来。
- **三跳查找全部参数化**(cmdline 驱动,默认值即现状行为):

  | 跳 | 参数(暂名) | 默认 | 查找范围 |
  | --- | --- | --- | --- |
  | shim → 加载对象 | `kernel.phys.load` | "physload" | OBI item(按名) |
  | physload → 下一模块 | `kernel.phys.next` | "physboot" | kernel package BootFS |
  | physboot → 内核镜像 | `kernel.name` | "vmours" | kernel package BootFS 目录 |

  参数名是提案,实施前可改;语义(默认值、查找范围)已定。

## 1. 目标 .obi 内部树

```
kernel.dev.obi:
  [0] physload      OBI item: type=EXEC, format=Elf(strip 后 ELF,
                      保留 .note.gnu.build-id 备将来配对)
  [1] cmdline       OBI item: type=CONF, format=Raw(镜像级 cmdline)
  [2] kernel        OBI item: type=KSTR(StorageKernel), format=BootFs, lz4
      └─ BootFS(解压后,**顶层无 "ours/" 前缀**——"ours" 只是 GN 侧
         kernel_package 的目标名,不是路径层级):
          physboot                  ← kernel_package("physboot"),prefix=""
          vmours/image.elf          ← kernel_package("ours"),prefix=""
          vmours/code-patches
          userboot                  ←(后续,留空)
          libours.so                ←(后续,留空)
          version-string.txt        ←(后续,留空)
```

- **KSTR 只有一个**:physboot 与 ours 两个 kernel_package 合并进同一压缩
  BootFS item。分层说明:"ours" 真实存在于构建侧(kernel_package("ours")
  负责 收 vmours/userboot/libours...),但 BootFS 是扁平命名空间,合并后
  GN 包边界消失;physboot 的消费契约只是"按 kernel.name 找目录,内含
  image.elf + code-patches",不感知文件的来源包。
- cmdline 是镜像级条目,位于压缩包之外(现状挂在 kernel_package("ours")
  deps 里属于错挂,顺手修正)。

## 2. 核心类设计(用户确认:头文件已标注的几个对象)

链路挂在三个既有视图类上,职责不动、能力补齐:

- **`obi::BootObi`**(kernel/lib/obi/include/obi/boot-obi.hpp)
  ——最外层对象(kernel_image GN 模板产物的视图)。持有/暴露三样:
  `executable()`(item 0,physload,新增访问器)、`command_line()`、
  `kernel_package()`。`from()` 收紧为恰好一个 StorageKernel。
  shim 与 physload 共用它读 OBI。
- **`obi::KernelPackage`**(kernel/lib/obi/include/obi/kernel-package.hpp)
  ——kernel_package GN 模板产物的视图(解压后的 BootFS)。
  **包内文件命名词汇归它所有**,作为 static constexpr:
  `kImageElf = "image.elf"`、`kCodePatches = "code-patches"`。
  消费方组合"目录名 + 常量"取文件,不自行硬编码文件名。
  GN 侧 kernel_elf_image 的 destination 与常量对齐(建议去掉 `.bin` 后缀,
  树即契约)。
- **`ElfImage`**(kernel/phys,elf-image.cpp)
  ——ELF 镜像装载器。physload 用它装 physboot 模块(ET_DYN + relative
  重定位),physboot 用它装 vmours;shim 需要的"把 physload ELF 装到内存"
  复用同一实现(链接 phys 库子集,不另写一份)。

参数化载体仍是 `BootOptions`(kernel/phys):`next_module` + 新增
`kernel_name`,shim 侧自行解析同一 cmdline(它在 physload 之前跑,拿不到
BootOptions 实例)。

## 3. 现状问题清单

1. **physboot 不是真 module**:physboot.cpp 在 `source_set("phys")` 里,与
   physload、适配器同 ELF(Phase A 直调)。`physload_module()` 模板零实例化。
2. **kernel_package("physboot") 是空包**:唯一 dep 是 `:physload.module`
   source_set,不产生 distribution_entries。
3. **KSTR 会分裂**:实测 rsp 中 ours 与 physboot 两个 kernel-storage 块被
   `--reset` 隔开(flush+clear);今天 physboot 包为空被 flush 静默丢弃所以
   只有一个 KSTR,一旦有内容即裂成两个。`BootObi::kernel_package()` 取
   "第一个",顺序脆弱。
4. **查找目标写死**:physboot 的 `KERNEL_NAME = "ours/vmours"` 是编译期常量;
   `BootOptions::load_target`("kernel_main")是既不解析也不使用的死配置。
5. **shim 缺主链路**:shim.cpp 调 `obi_main()`,全仓库无定义(drift);
   "从 OBI 装 physload"的能力不存在。
6. **handoff 半空**:`kernel_storage_base/size` 恒 0,physboot 靠
   `obi_package_base` 整包重解析;scratch 是静态 8MiB 数组,而非按 KSTR
   `memory_size` 从 BootMem 分配。
7. **--bootable 未实现**:obit 解析该参数但不校验,拓扑错误只能开机 panic。
8. **空 cmdline 打不出 CONF item**:pack.cpp 只有 `--entry` 建 item,
   `--type=cmdline` inert;dev `args = []` → 镜像里没有 cmdline 节点。
9. **vmours 是裸文件**:`ours_kernel_has_code_patches = false`
   (build/config/BUILDCONFIG.gn:85)→ 打包成 "ours/vmours",非目标树的
   image.elf/code-patches 子树形态。

## 4. 设计

### 4.1 打包侧(build/obi + obit)

- **同 files_type 合并、延迟 flush**:accumulators 跨 `--reset`/
  `--files-type` 存活,`--files` 按(prefix 快照, files_type)计入对应
  accumulator,rsp 末尾统一 flush;顺序 StorageBootFs 在前、KSTR 殿后。
  prefix/compressed 语义随块,不变。
- **`--type=executable <elf>` 输入**:校验 ELF 魔数后以 EXEC/Elf item 入包,
  item 名取文件基名。physload 的 item 0 走这条路。
- **`--bootable` 校验**:item 0 必须 Executable+Elf;恰好一个 StorageKernel;
  违者构建期报错。
- **kernel_cmdline 空态**:args 为空时合成一条空串 entry,保证 cmdline
  节点常在(目标树契约)。

### 4.2 GN 拓扑

```gn
# kernel/products/image/dev/BUILD.gn
kernel_package("ours") { prefix = ""; deps = [ vmours... ] }  # 无路径前缀;cmdline 挪出
kernel_cmdline("cmdline") { args = [] }             # 上移到镜像级
kernel_image("dev") {
  deps = [ "//kernel/phys:physload.first-item",     # item 0(EXEC/Elf 注册)
           ":cmdline",
           ":ours",
           "//kernel/phys:physboot" ]               # kernel_package, prefix=""
}
```

- `phys_executable("physload")` 增加 ELF 分发:strip 一份(保 build-id note)
  自注册 `--type=executable`;raw `.bin` 是否继续发布随 shim 改造结论定
  (shim 成为唯一 GRUB 入口后,raw 分发仅留调试用途)。
- vmours 落子树形态:翻 `ours_kernel_has_code_patches`(或显式
  has_patches),destination 用 `image.elf`/`code-patches`(与
  KernelPackage 常量对齐)。

### 4.3 x86 shim(新增工作项)

shim 改造为最小加载器,职责收敛为:

1. 解析 Multiboot2 参数 → 拿到 OBI module 区间。
2. `BootObi`/`ContainerView` 读 OBI;cmdline 若含 `kernel.phys.load` 则取值,
   否则默认 "physload";按名取 EXEC/Elf item。
3. 用 ElfImage(phys 库子集)装载该 ELF,跳转其入口,传 OBI 区间。
4. 不做:内存初始化、KSTR 解压、模块加载——这些归 physload。

shim 自身仍是 GRUB 直接加载的 Multiboot 映像(带 Multiboot 头),不在 OBI 内。

### 4.4 phys 拆分与模块加载

```
source_set("physload.core")   physload.cpp、boot-options、memory、aspace、
                               console、panic、raw-image、elf-image.cpp、
                               module-loader.cpp(新)
physload_module("physboot")   physboot.cpp + kernel-boot.cpp +
                               kernel-package-loader.cpp;ET_DYN,
                               install_name "physboot" → bootfs 裸名
source_set("physload.module") 标准适配器,只链进模块
phys_executable("physload")   deps = [":physload.core"] + item 0 ELF 注册
kernel_package("physboot")    deps = [":physboot"],prefix = ""
```

physload Stage 4/5(真加载):

1. KSTR item 按 `memory_size` 从 BootMem 分配 scratch 并解压(所有权随
   handoff 移交,不释放)。
2. BootFS 按 `kernel.phys.next` 取模块;未命中告警(含名字)回退默认
   "physboot";连默认也缺 → panic。
3. `ElfImage` 装载 ET_DYN、apply relative relocations("打补丁"的落点:
   重定位必做;若模块带 code-patches(has_patches 打开时)则一并应用),
   入口 = `load_bias + e_entry`(适配器经链接脚本约定把 `phys_load_handoff`
   布置为 e_entry)。
4. 尾调用 `phys_load_handoff(PhysLoadHandoff)`。

入口 ABI 两层:

- `phys_load_handoff`(extern "C"、NO_MANGLE、NO_RETURN)——physload 调的
  唯一入口,由 physload.module 适配器提供:安装服务(console/allocation/
  symbolize)→ `arch_on_phys_load_handoff()` → 调模块主体。
- 模块主体 `phys_module_main`(每模块一份,同 ELF 内 C++ 链接;删掉
  `phys_load_module_main` 的错误 NO_MANGLE 注释)。

### 4.5 参数化(BootOptions)

- `load_target[16]{"kernel_main"}` 删除,替换为:
  - `char next_module[64]{"physboot"}` ← `kernel.phys.next`(physload 用);
  - `char kernel_name[64]{"vmours"}` ← `kernel.name`(physboot 用,
    目录名语义;取文件组合 `KernelPackage::kImageElf`/`kCodePatches`)。
- 解析在 Stage 2(早于 KSTR 解压);未知 token 留给后续阶段。
- shim 侧独立解析 `kernel.phys.load`(默认 "physload"),未命中该名字的
  item 时告警回退默认(与 next 同语义)。

### 4.6 PhysLoadHandoff 落实

`kernel_storage_base/size` = 解压后 BootFS 区间(physboot 不再重解析外层
OBI);`bootmem` 填实 EarlyMem 回调表;`obi_package_base/size` 保留(内核侧
可能还要 memory-config 等)。physboot 删除静态 8MiB scratch。

### 4.7 后续工作(本期不做)

- build-id 配对(kernel_elf_interp 接线)。
- userboot / libours.so / version-string.txt 进 kernel_package("ours")。

## 5. 实施顺序(每步可独立构建验证)

1. obit pack:合并 flush + `--type=executable` + --bootable 校验 + 空 cmdline
   落 item(+ cli 测试);BootObi 恰一 KSTR + `executable()`。
2. image/dev GN 重排:cmdline 上移、physload ELF 入 item 0、physboot 包
   prefix=""、vmours 子树形态 → `obit list` 呈现目标树。
3. BootOptions 参数化:next_module + kernel_name + KernelPackage 常量;
   删 load_target。
4. phys 拆分:physload.core / physboot 模块 / 适配器归位;修 obi_main 漂移。
5. physload 真加载:KSTR 解压进 BootMem、模块装载(重定位+可选补丁)、
   入口尾调用;handoff 落实;physboot 瘦身。
6. x86 shim 改造:OBI → 按 `kernel.phys.load` 取 physload ELF → 装载跳转。
7. 回归:bringup 构建 + obit list 快照 + 既有单测 + QEMU/GRUB 冒烟。

## 6. 实测中间产物(out/bringup.x86,2026-08-18)

kernel_image 中间产物链:

1. 层 1/2 JSON:`gen/kernel/products/image/dev/ours.fini.dist.partial`
   `[{"destination":"vmours","source":".../vmours.with-tests.image.elf"}]`;
   cmdline partial 为 `[]`。
2. 层 3 FINI:`destination=source` 文本行,obit `--files` 的输入。
3. rsp(`gen/.../dev.obi.rsp`,GN→obit 契约):
   ```
   --reset --type=cmdline --files .../cmdline.fini
   --reset --files-type=kernel-storage --prefix=ours --compressed=lz4 --files .../ours.fini
   --reset --files-type=kernel-storage --prefix=physboot --compressed=lz4 --files .../physboot.fini
   ```
4. 最终 `.obi` 陈旧树未产出;obit 无 Fuchsia 式 .json 内容清单。

实测结论:

- rsp 顺序 = 依赖树后序 DFS(cmdline、ours、physboot)。physload 作首个
  dep → rsp 首块 → 延迟 flush 下 item 0;bootable 校验固化为断言。
- physboot 块 `--prefix=physboot`(默认 prefix=target_name),prefix="" 需显式。
- 空 cmdline 无 CONF item(问题清单 #8)。
- `ours_kernel_has_code_patches = false` → vmours 裸文件(问题清单 #9)。

## 7. 事实依据(代码位置)

- 双 KSTR 隐患:`kernel/products/image/dev/BUILD.gn`(kernel_image deps
  ":ours" + "//kernel/phys:physboot");rsp 状态机:`kernel/lib/obi/cli/pack.cpp`。
- 空的 physboot 包:`kernel/phys/BUILD.gn:155`。
- Phase A 直调:`kernel/phys/physload.cpp:98`;死配置
  load_target:`kernel/phys/include/ours/phys/boot-options.hpp:15`。
- obi_main 无定义:`kernel/phys/include/ours/phys/init.hpp:48` vs
  `kernel/phys/physload.cpp:33`;shim 调用点:
  `kernel/arch/x86/phys/boot-shim/shim.cpp:29`。
- raw 回退包 EXEC item:`kernel/lib/obi/cli/pack.cpp:145`;
  embed_in_obi:`kernel/phys/phys_executable.gni:29`。
- interp 机制(本期闲置):`kernel/phys/kernel_elf_interp.gni`、`elf-interp.S`。
- code-patches 开关:`build/config/BUILDCONFIG.gn:85`;destination 字符串:
  `kernel/phys/kernel_elf_image.gni`。

## 8. 实施记录(2026-08-20)

### 8.1 第 1 步(obit 能力)— 完成,全部测试绿

- `kernel/lib/obi/cli/pack.cpp`:rsp 状态机改为"同 files_type 跨 --reset 累计、
  rsp 末尾统一 flush(StorageBootFs 先、StorageKernel 殿后)";新增
  `--type=executable <elf>`(ELF 魔数校验 → EXEC/Elf item,名字取基名去首
  扩展名);`--bootable` 落地(item 0 必须 Executable;恰好一个 StorageKernel;
  违者构建期失败)。
- `kernel/lib/obi/include/obi/boot-obi.hpp`:`from()` 恰一 KSTR(0 → NotFound,
  >1 → AlreadyExists);新增 `executable()` 访问器(item 0)。
- `kernel/lib/obi/include/obi/kernel-package.hpp`:`kImageElf`/`kCodePatches`
  static constexpr 落位(§2 的单一事实源)。
- `build/obi/obi_input.gni`:type=cmdline 无 entries 时合成 `--entry=`(空
  cmdline 也落 CONF item)。
- 测试:pack-smoke.py 新场景(合并/executable/bootable 正反例/空 cmdline)、
  package-builder-test 新增 3 条 BootObi 断言;`kernel/lib/obi:tests` 组改挂
  `($host_toolchain)`(此前默认工具链下编译必败,从未真正入构建)。
- 佐证:dist-smoke.obi / product-smoke.obi 用新 obit 打包通过。

### 8.2 第 2 步(GN 拓扑)— 完成至 rsp 验证

- `phys_executable.gni`:新增 `obi_executable_item` 参数——phys 工具链内新增
  strip 动作(`llvm-objcopy --strip-sections` → `physload.elf`,保 program
  headers 与 PT_NOTE 内 build-id),组 metadata 自注册
  `--reset --type=executable <elf>`;默认工具链组补上跨工具链依赖(修复了
  "空组导致 physload 从未被构建"的存量缺口)。
- `kernel/phys/BUILD.gn`:physload 开启 `obi_executable_item`;physboot 包
  `prefix = ""`。
- `image/dev` + `image/dev.dbg_1`:cmdline 上移镜像级,kernel_image deps 重排
  `[physload, cmdline, ours, physboot]`。
- vmours 子树形态:`kernel_executable.gni` 支持透传 `has_patches`;
  `products/bin` 两个内核 `has_patches = true`;`kernel_elf_image.gni`
  destination 去 `.bin` 后缀(与 `KernelPackage::kCodePatches` 对齐)。
- **rsp 已验证为目标拓扑**(`out/bringup.x86/.../dev.obi.rsp`):
  `--type=executable physload.elf` 首块 → 空 cmdline → 两个无前缀
  kernel-storage 块(合并为单 KSTR)。
- 顺手修:`kernel/lib/libc/string.cpp` 缺 `#include <ours/compiler.hpp>`
  (NO_MANGLE 未定义的存量编译错,一行修复)。

### 8.3 第 3 步(BootOptions)— 完成

`load_target` 删除;`next_module[64]{"physboot"}` ← `kernel.phys.next`、
`kernel_name[64]{"vmours"}` ← `kernel.name`,超长值 → InvalidArgument;
boot-options-smoke 增加默认/覆盖/超长三组断言(host 工具链全绿)。

### 8.4 Console 自动注册重设计(2026-08-21,用户授权)— 完成

用户目标:console 的自动注册。设计落在仓库既有 ccrt 基建上:

- **注册契约**(`kernel/phys/include/ours/phys/console.hpp`):console 实现在
  自己的翻译单元里声明匿名命名空间静态实例(无注册宏、无中心清单);其
  构造函数调用 `Console(char const*)` 把对象挂进 **constinit** 文件作用域
  侵入式链表(常量初始化 ⇒ 与任何 console 构造之间无动态初始化顺序问题,
  无 __cxa_guard 依赖)。
- **驱动**:镜像入口调用 `ccrt::init_global_static_objects()`(走
  `.ccrt_init` 段 = `.init_array`,phys.ld/kernel.ld 均已收集)跑构造;
  `init_early_console()`(init.hpp 声明,console.cpp 定义)对全部已注册
  console 调 `activate()`。physload 的 `obi_main` 入口已接入;boot-shim
  原本就在调。
- **输出面**:`Console::write_all(char const*, u32)` 静态成员扇出;
  `vprint` 委托它(修掉原来引用不存在 `CONSOLE_LIST` 的 bug)。
- 实证:链接产物 `.ccrt_init` 0x18 字节(3 个初始化器,含
  `_GLOBAL__sub_I_serial_console.cpp` / `_..._vga_console.cpp`),
  `g_serial_console`/`g_vga_console` 落在 bss。

### 8.5 physload 链通(2026-08-21)— 完成

phys 工具链下 `physload` ELF 成功编译链接(x86-64、3 个 LOAD 段、
build-id),`--strip-sections` 后的 `physload.elf`(item 0 材料)产出。
注意:宿主 arm64 环境下 ninja 的链接步骤因 clang++ 驱动回退到宿主 g++
而失败,用 `ld.lld` 手动等价链接验证;dev-common 容器内构建无此问题。
strip 会丢 build-id note(无 PT_NOTE 覆盖),已在 phys_executable.gni
注释中如实记录(build-id 配对为既定的后续项)。

为链通顺手修复的存量漂移(全部为机械修复,非行为变更):

| 文件 | 修复 |
| --- | --- |
| `kernel/lib/libc/string.cpp` | 补缺失 `#include <ours/compiler.hpp>`(NO_MANGLE) |
| `kernel/phys/include/ours/phys/aspace.hpp` + `aspace.cpp`(两处) | 成员名迁移尾巴:`lower_pgd_` 等 → `m_lower_pgd` 等 |
| `kernel/phys/include/ours/phys/{init,physload,kernel-boot}.hpp`、`arch/x86/phys/legacy-boot.hpp` | `struct Aspace` 前向声明 → `class`(Wmismatched-tags) |
| `kernel/phys/memory.cpp`、`eftl/include/eftl/**`(12 个头) | `ours::u32` 等限定修正(u32 在全局命名空间) |
| `kernel/phys/include/ours/phys/print.hpp` | 定义缺失的 `OURS_DEBUG`(由 KERNEL_DEBUG_LEVEL 推导) |
| `kernel/phys/BUILD.gn` | **在用的 phys 工具链套件补挂 `phys.config`**(此前 PHYS_ENVIRONMENT 从未定义——print 走宿主路径、itanium 的 PHYS 段被条件编译掉);去掉与 kernel.config 重复的 KERNEL_DEBUG_LEVEL |
| `kernel/lib/ccrt/compiler/itanium.cpp` | PHYS 段补 `operator new/new[]`(分配即 panic) |
| `system/public/ours/panic.hpp` | 模板 `vpanic(String, FormatArgs)` 只声明无定义:`panic()` 重写为调用点 `std::vformat_to` 到栈上定界 sink(无堆);PHYS 环境分流为"丢参数只打文本"(与 print.hpp 的 PHYS 语义一致,避免把 std::format 分配机制拖进引导映像);新增 `vpanic(string_view)` 声明 |
| `kernel/top/panic.cpp` | 补 `vpanic(string_view)` 定义(kernel 侧) |
| `kernel/phys/physload.cpp` | 两参 `phys_main` 改名 `obi_main`(修 init.hpp 声明/shim 调用无定义的漂移;asm 的 `phys_main` 由 shim 的一参版提供) |
| `kernel/phys/physboot.cpp` | `module_main` 改名 `phys_load_module_main`(适配器调用的名字) |
| `eftl/elf/observable.hpp` | 内层模板参数名遮蔽外层 `Traits` → `TraitsT` |
| `eftl/elf/load_policy.hpp` | `make_plan(Binary const&)` → `make_plan(Image const&)`(类型擦除 Binary 无 elf_file(),所有调用方传的都是 BasicImage) |

### 8.6 剩余阻塞

仅剩 kernel 侧(vmours 链)的存量编译错(sched/gktl/profiles/logz4 等,
非打包链问题)。修通后 `kernel.dev.obi` 即可完整打包并 `obit list` 验证
目标树。第 4 步(phys 拆分与真加载)、第 5 步(shim 改造)的编码前提
(phys 栈可编译链接)已就绪。

### 8.7 验证状态汇总(2026-08-21)

| 验证项 | 状态 |
| --- | --- |
| obi 单测/smoke(含新场景) | ✅ 全绿(host) |
| BootOptions 解析 | ✅ smoke 绿 |
| rsp 拓扑(physload item0/空 cmdline/单 KSTR) | ✅ 已验 |
| physload ELF 编译+链接+strip(含 console 自动注册段) | ✅ 已验(ld.lld 手动链接) |
| kernel.dev.obi 全树 | ⛔ 待 kernel 侧存量修复 |
