# 仓库状态审核 + 提交拆分方案（未执行）

日期：2026-08-18。起因：想给 ACPI 重构开 PR，发现仓库状态不允许。
状态：**已暂停**，用户决定暂不处理。没有提交、没有推送、没有 PR。

## 为什么开不了 PR

- `origin` 上只有 `main` @ `1387a1a`；`feat/arch-lib-restructure`（45 个提交）从未推送。
- `HEAD` 仍是 **Bazel 时代**的树（`.bazelrc` / `BUILD.bazel` / `kernel/lib/acpi/CMakeLists.txt`）。
- 整套 GN 构建系统（`build/cpp/`、`build/toolchain/`…）**全部未跟踪**。
- 工作区有 ~1400 个未提交路径。ACPI 的改动一半落在这些未跟踪目录里，
  单独提交会得到一个引用不存在文件的、编不过的 PR。

## 审核中已修掉的 5 个问题（改动仍在工作区，未提交）

1. `.gitignore`：`prebuilt/` 没被忽略 —— `git add -A` 会提交 1784 个工具链文件。已加。
2. `.gitignore:57`：写的是 `.__pycache__`（多一个前导点），规则从未生效，
   19 个 `.pyc` 会被提交。已改为 `__pycache__/` + `*.pyc`。
3. `.gitmodules`：4 个内嵌 git 仓库未声明，`git add` 会把它们变成悬空 gitlink。已补：
   `system/lib/boost.container`、`vendor/cxxopt`、`vendor/googletest`、`vendor/nlohmann_json`。
4. `kernel/arch/x86/BUILD.gn`：`cpp_interface` 只对 `headers =` 参数加 `include_dir` 前缀，
   这里用了 `public =`，24 个头文件被解析成不存在的 `//kernel/arch/x86/arch/...`。
   已改为 `headers =`。
5. `kernel/lib/obi/tests/BUILD.gn`：`cli-smoke.cpp` 刻意 `#include "../cli/main.cpp"`
   （配合 `#define main obit_main`），不能真加依赖否则 main 重复链接。已加
   `check_includes = false`。

修复前后：`gn check` 从 73 个错误变为 x86 / arm / riscv **全部通过**。

## 未处理（用户自己负责）

`system/lib/{lockdep,concurrent,bits}` —— `.gitmodules` 声明为 submodule 但目录里
没有 `.git`，装的是未版本化的工作（`lockdep` 有 11 个文件，构建在用）。
用户表示会自己推到各自仓库。**本次一个字没动这三个路径。**

## 分支构建现状

`ninja -C out/bringup.x86 -k 0`：289 个目标中 **200 个通过，89 个失败**，
全部集中在 kernel proper，属于这条分支自身未完成的部分：

- `NO_MANGLE` / `FORCE_INLINE` / `WEAK_LINK` 未定义（`ours/compiler.hpp` 没被引入）
- `lockdep/state-saver.hpp` 找不到（GN 依赖缺失，文件本身在磁盘上）
- `ustl::move` / `ustl::NumericLimits` 不存在（ustl API 漂移）
- `arch::PageTable` / `arch::AddrSizeCpuIdObserver` 不存在（arch 重构未完成）

`build/` 系统、host 测试、smoke 测试、`kernel/phys`、各库均正常。

## 提交拆分方案

暂存后 git 识别出 **91 个改名**（`kernel/main`→`kernel/kernel` 30、`kernel/arch` 内部 29、
`kernel/include`→`kernel/mm` 14、`kernel/main`→`kernel/mm` 11），所以第 7 组的实际
diff 远小于文件数。

| # | 提交 | 文件数 |
|---|---|---|
| 1 | `chore: ignore prebuilt, fix __pycache__ pattern, declare vendored submodules` | 4 |
| 2 | `vendor: boost.intrusive / boost.container / cxxopts / googletest / nlohmann json` | 214 |
| 3 | `build: replace Bazel/CMake with GN` | 157 |
| 4 | `feat(system/public): ABI 头文件整理` | 20 |
| 5 | `feat(system/lib): ustl 扩充` | 234 |
| 6 | `feat(kernel/lib): eftl / obi / gktl / ktl / logz4 / ccrt / code-patching` | 207 |
| 7 | `refactor(kernel): kernel/main→kernel/kernel、mm 归位、arch/x86 重构` | 483 |
| 8 | `refactor(lib/acpi): 重新设计 ACPI 库` | 31 |
| 9 | `docs/agents: 文档与 skill 更新` | 54 |

**取舍说明**：`kernel/platform/pc/{acpi,hpet,interrupt}.cpp`、`kernel/arch/x86/mm/srat.cpp`
这些为 ACPI 改过的文件在 git 里是全新文件。方案里把它们归入第 6/7 组（按结构归属），
好处是第 8 组的 diff 是干净的「ACPI 库重新设计」对照已跟踪历史，正是 PR 里该看的东西；
代价是对那几个新文件的 ACPI 适配混在第 7 组里看不出来。

另：14 个文件是 CRLF 换行，提交时会被 git 归一成 LF
（`kernel/lib/{gktl,ktl,libc,logz4,lz4}`、`kernel/platform/pc/init.cpp`）。
