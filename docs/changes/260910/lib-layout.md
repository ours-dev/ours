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
