# 库目录结构 — 待修

---

## 1. 两个 CMake 孤儿文件

`kernel/lib/arch/tests/CMakeLists.txt`、`kernel/lib/ktl/CMakeLists.txt`。
本仓库的构建是 GN，除 vendored 的 `system/lib/boost.intrusive` 外没有任何 CMake 使用者，
也没有工具会读它们。确认无用后删掉。

## 2. `run_tests.sh` 是 shell 脚本

`kernel/lib/code_patching/test/run_tests.sh`。本仓库规定工具脚本用 Python 而不是 shell
（`scripts/` 下已全部是 `.py`）。应当改写成 Python，或并入 GN 的单测目标。

## 3. 存量库仍未对齐结构

`tests/` → `test/` 已完成。剩下的：

- **`src/` 缺失**：只有 `kernel/lib/gktl` 和 `kernel/lib/kconcurrent` 有。
  `kernel/lib/prof` 的源文件在 `prof/`，`kernel/lib/unwind` 的在 `unwind/`，
  `kernel/lib/ccrt` 的在 `compiler/`。其余多数是纯头文件库，本来就不需要 `src/`。
- **`.gitignore` 缺失**：28 个库无一有。
