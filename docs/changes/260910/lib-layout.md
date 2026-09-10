# 库目录结构对齐

格式见 `docs/changes/TEMPLATE.md`。目标结构（`tools/templates/cpp_library` 生成的那一套）：

```
include/<name>/   公开头文件
src/              源文件
test/             宿主侧单测
BUILD.gn
.gitignore
.clang-format
```

---

## 1. `tests/` 统一改成 `test/`（2026-09-10）

**文件**
- 7 个库目录改名：`kernel/lib/{ccrt,code_patching,eftl,obi,prof,unwind}`、`system/lib/hsien`。
- `kernel/lib/arch` 原来 `test/` 和 `tests/` 并存，把 `tests/paging_test.cpp` 并入 `test/`。
- 46 处 GN label 更新：obi 10 处绝对 label，eftl 32 / prof 2 / unwind 2 处相对 label。

**症状**

无。纯结构不一致：4 个库用 `test/`，8 个用 `tests/`。

**原因**

没有单一来源，各写各的。现在 `tools/templates/cpp_library` 定了 `test/`，存量要跟上，
否则模板生成的和现有的两套并存，等于没定。

**改法**

`git mv` 改名，然后改两类引用——注意**只改目录引用，不动目标名**：

```gn
# 之前                                  # 之后
"//kernel/lib/obi/tests:obi-smoke"     "//kernel/lib/obi/test:obi-smoke"
"tests:format($host_toolchain)"        "test:format($host_toolchain)"

group("tests") { ... }                 # 不动：这是目标名，不是目录
```

`group("tests")` 是每个库对外暴露的单测汇总目标，和目录同名纯属巧合；把它一起改会破坏
`//kernel/lib/obi:tests` 这类外部引用。

**验证**

构建通过；`kernel/lib/{prof,unwind,obi}:tests` 三个 group 能构建。
`kernel/lib/eftl` 不在 bringup 产品的构建图里（GN 不加载它的 `BUILD.gn`），所以那 32 处
改动**没有被构建验证**，改用静态核对：`eftl/BUILD.gn` 引用的 32 个目标名与
`eftl/test/BUILD.gn` 定义的 32 个逐一对上，无缺失。prof / unwind 同样核对过。

QEMU dbg 启动无回归：无断言、13.5 万行线程输出。

**故意没做**

- `kernel/lib/arch/tests/CMakeLists.txt` 和 `kernel/lib/ktl/CMakeLists.txt` 留着没删。
  本仓库已不用 CMake（除 vendored boost 外只剩这两个孤儿），但删文件不属于「对齐结构」，
  单独记进待修。
- `kernel/lib/code_patching/test/run_tests.sh` 留着没动，同样记进待修——本仓库的脚本
  规定用 Python 而不是 shell。

---

## 2. 源文件进 `src/`、`.gitignore` 补齐（2026-09-10）

**文件**
- 三个库把自带名字的源目录改成 `src/`：`prof/prof/` → `prof/src/`、
  `unwind/unwind/` → `unwind/src/`、`ccrt/compiler/` → `ccrt/src/`。
- 八个库把散在根目录的源文件收进 `src/`：`acpi`(3)、`heap`(3)、`ktl`(1)、`lz4`(1)、
  `syscall`(4)、`userabi`(2)、`hsien`(2)、`logz4`(3)。
- 三个 phys 库把 `*_smoke.cpp` 归进 `test/`：`bootmem`、`bootshim`、`multiboot`。
- 28 个库补上 `.gitignore`。

**原因**

同第 1 条：模板定了结构，存量要跟上，否则等于没定。

**改法**

`git mv` + 更新 `BUILD.gn` 的 `sources`。一个坑：**同一个源文件名会在多个目标里出现**。
`kernel/lib/acpi` 的三个源文件被 `source_set` 和三个单测可执行文件各列一遍，
`"table_set.cpp"` 在文件里出现 4 次：

```gn
executable("numa-test") {
  sources = [
    "numa.cpp",        # ← 单测直接编库的源文件，不是只依赖 :header
    "table_set.cpp",
    "test/numa_test.cpp",
  ]
}
```

所以替换必须是全量的，按「第一处」改会留下半数指向旧路径。

**故意没做**

- **没有给纯头文件库造空的 `src/`**。`bootfs`、`clock`、`mtl`、`derive`、`ustl`、
  `eftl`、`arch` 等本来就没有 `.cpp`，造个空目录既没内容、git 也存不住空目录
  （非要存就得塞占位文件）。同理没给无单测的库造空 `test/`。
  **结论：结构标准是「有什么就放在对的地方」，不是「每个目录都必须存在」。**
- **独立产物不算库源码，留在原处**：`obi/cli/`（obit 命令行工具）、`prof/cli/` 与
  `prof/examples/`、`userabi/userboot/` 与 `userabi/vdso/`（跑在用户态的程序）、
  `hsien/tools/tests/fixtures/`（工具自己的测试夹具）。它们各有各的构建目标，
  塞进库的 `src/` 会把「库」和「用这个库构建出的程序」混为一谈。
- `kernel/arch/{arm,riscv,x86}/lib/arch` 没有 `BUILD.gn`——它们是纯头文件的架构接缝，
  由 `kernel/lib/arch` 统一声明。是否要各自独立成目标是另一个问题，记进待修。

**验证**

构建通过；QEMU dbg 启动无回归：无断言、15.7 万行线程输出。
