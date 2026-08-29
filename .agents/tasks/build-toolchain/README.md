# build/toolchain 重构（Fuchsia 模式，单机构建）

状态：已完成并验证。日期：2026-08-17。

## 目标

以 Fuchsia build/toolchain 为蓝本重构 Ours 的构建工具链，聚焦单机构建
（明确排除 RBE/CCACHE），要求：
- 每个config只表达一个关注点（不允许一个大数组塞一堆flag）
- Clang 与 GCC 双编译器，默认 Clang；工具链路径默认取 prebuilt/
- 集成 Rust（GN 构建 + rust-analyzer 桥接）

## 已完成内容

### Phase 1a/1c — 编译器统一管理（build/config/compiler/）
- `compiler.gni`：`cxx_compiler` build arg（"clang"|"gcc"），导出 is_gcc/is_clang
- `clang.gni`：clang_tool_dir 默认 `//prebuilt/clang/llvm-22/bin`；
  `clang_version_string` 从 `.versions/clang.cipd_version` 读取（Fuchsia 模式）
- `gcc.gni`：gcc_tool_dir 默认 `//prebuilt/gcc/${host_os}-${host_cpu}/bin`；
  GNU linker 合成符号表
- `rust.gni`：rust_tool_dir 默认 `//prebuilt/rust/stable/bin`；
  `standard_ours_rust_targets`（x86→x86_64-unknown-none 等）；
  rust_panic/rust_edition 默认值
- `ifs.gni`：统一的 `ifs_extra_switches`（gcc 模式排除 GNU 合成符号）
- BUILDCONFIG.gn：import 切换到新位置；is_gcc/is_clang 改由 cxx_compiler 驱动
  （原先基于 variant tags）；删除旧 build/config/clang/clang_prefix.gni、clang.gni
- prebuilt 新增：clang 版本戳、gcc/linux-arm64（软链 Fuchsia prebuilt GCC 15.2.1
  ELF 交叉工具，含 x86_64/aarch64/riscv64-elf）、rust/stable（软链 rustup 1.95）、
  ninja（软链 Fuchsia prebuilt）

### Phase 1b — ours_toolchain() 双编译器 + Rust 工具
- `_use_gcc = is_gcc && !_is_host`：host 工具链恒为 Clang；GCC 模式下
  kernel/phys 工具链用 `<cpu>-elf-{gcc,g++,gcc-ar}`，无 --target（GCC 自带三元组）
- LLVM binutils（llvm-strip 等）两种模式都走 clang_tool_dir
- elf_linker 现在总是经 gcc_tool_dir 解析（GNU 工具）
- 新增 5 个 rust tool()：rust_bin/rust_rlib/rust_staticlib/rust_cdylib/rust_macro
- toolchain_tools 契约在 GCC 模式指向 gcc 目录 + `<cpu>-elf-` 前缀

### Phase 2 — compiler_config() 模板（build/config/compiler_config.gni）
Fuchsia 完整版移植（c_family_flags/linker_flags/llvm_flags/linker_llvm_flags/
rust_linker_*/rust_target_features），去掉内核 Rust 自定义 target JSON 编辑
（ours 用 stock freestanding target）。

### Phase 3 — flag 组织重构（每 config 一个关注点）
- `default_warnings` → warn_common + warn_strict + warn_suppress
- `compiler` → color_diagnostics + no_common + kernel_libcxx_include
- `hermetic_code_blob` → hermetic_freestanding_pic + hermetic_no_startup +
  hermetic_no_runtime + hermetic_linker_policy + hermetic_linker_script
- x86 → x86-isa（-march/-mtune）+ x86-kernel-mode（-mno-red-zone/-mno-mmx/-mno-sse）
- arm 保持 armv8-a（对齐 Fuchsia kernel 分支；userspace 扩展等 userspace 落地再加）
- riscv 新增非 PIC 时 `-msmall-data-limit=0`
- Clang 专属 flag 加 is_clang 守卫（color_diagnostics/--icf/-nostdlib++/
  -ftemplate-backtrace-limit/-Wno-thread-safety-attributes）

### Phase 4 — Rust GN 模板（build/rust/）
- `rust_artifact.gni`：中央模板（crate_name/crate_root/edition/features → rustflags）
- 薄封装：rust_executable / rust_library / rust_test / rust_macro /
  rust_staticlib / rust_cdylib
- rust-analyzer：`gn gen --export-rust-project` → out/<dir>/rust-project.json
  （tools/set.py 已默认带上）。VSCode 里把 rust-analyzer.linkedProjects 指向它。
- 冒烟：rust-smoke-lib（rlib）+ rust-toolchain-smoke-test（bin+extern）+
  rust-smoke-unit（--test harness），挂在根 default group

### Phase 5 — 工具链增强
- restat 接入：`restat_cc=true` 时 cc/cxx 命令前置 restat_cacher.py。
  注意必须拼进 command 字符串——GN 的 command_launcher 属性不做 {{output}} 展开
  （踩过的坑，Fuchsia 也是拼字符串）
- link/solink/solink_module 挂 `//build/config:highmem` pool
  （深度 = 内存/32GB，最低 1）
- ifs_shared_library.gni 修复：删除不存在的 standard_fuchsia_cpus/
  clang_cpu_translations（gen 必炸的死代码），`--target` 改为
  `${cpu}-${ours_vendor}-${ours_os}-elf`
- exe.unstripped/ 结构：决定不引入——保持 Zircon 风格 `<output>.debug`
  后缀（link_output_suffix 契约的消费者依赖它）

## 验证记录
- 三架构 + gcc 模式 + restat 模式 gn gen 全通过
- host-toolchain-smoke-test：补齐了缺失的 .cc，编译/链接/运行 OK
- Rust：rlib 构建、bin 链接 extern、--test 单测、rust-project.json 依赖图全 OK
- restat：touch 源文件重编后字节不变 → mtime 保留 → 下游 LINK 被剪枝（实测）
- kernel C++ 编译失败是源码树本身缺失头文件（ours/config.hpp、
  arch/interrupt.hpp 等，arm/x86 同现），与本次改动无关（编译命令行逐 flag 对比等价）

## 2026-08-17 补充：host 工具链重复定义 bug（阻塞 gn gen）

GLM 中途额度耗尽暂停后，`gn gen` 报 `//build/toolchain/ours:host()` 重复定义。
根因：`build/toolchain/ours/BUILD.gn` 同时装了所有 `ours_toolchain(...)` 调用
**和** 一个普通 target `host-toolchain-smoke-test`（被根 `//BUILD.gn` 用显式
`($host_toolchain)` 后缀引用）。只要有任何东西在别的 toolchain 上下文里引用
同一份 BUILD.gn 里的普通 target，GN 就会把整份文件在那个上下文里重新执行一
遍——连带把里面所有 `toolchain()` 调用也重新跑一遍，产生重复定义。用
`print(target_name, current_toolchain)` 在 `ours_toolchain.gni` 里加诊断输出
实测确认：`host` 先在 `current_toolchain=kernel-x86`（正常，默认工具链解析
自身）下定义一次，又在 `current_toolchain=host`（被 smoke test 引用触发的
第二次加载）下试图定义第二次。

修复：把 `host-toolchain-smoke-test` 连同它的 `.cc` 挪到独立目录
`build/toolchain/host-smoke-test/`，不再跟任何 `ours_toolchain()` 调用共享
BUILD.gn；根 `//BUILD.gn` 的引用路径同步更新。这是一条通用规则，不只对这一
个 target 成立：**任何会调用 `toolchain()`（含间接经模板）的 BUILD.gn，都
不能再放会被显式指定 toolchain 后缀引用的普通 target。**

验证：`gn gen` 从报错变成 "Made 277 targets from 117 files"；
`host-toolchain-smoke-test`、`rust-toolchain-smoke-test`、
`kernel/lib/obi/cli:obit`（host 工具链）都编译+运行通过。`kernel_image("dev")`
的内核侧编译仍卡在下面"已知遗留"里那个 `-ffreestanding -nostdinc++` 模式下
`<cstdint>` 找不到的问题——这个跟本次修复无关，本来就存在。

## 已知遗留
- GCC 模式 kernel 端到端编译被缺失头文件阻塞（源码树问题）；flag 层已做
  is_clang 守卫
- kernel Rust 需 rustup 装 rust-std（x86_64-unknown-none 等）才能真正编译内核 crate；
  host Rust 开箱即用
- ifs_shared_library 暂无调用者，修复仅经 gen 验证
