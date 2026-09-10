# 库目录结构 — 待修

---

## 1. 两个 CMake 孤儿文件

`kernel/lib/arch/tests/CMakeLists.txt`、`kernel/lib/ktl/CMakeLists.txt`。
本仓库的构建是 GN，除 vendored 的 `system/lib/boost.intrusive` 外没有任何 CMake 使用者，
也没有工具会读它们。确认无用后删掉。

## 2. `run_tests.sh` 是 shell 脚本

`kernel/lib/code_patching/test/run_tests.sh`。本仓库规定工具脚本用 Python 而不是 shell
（`scripts/` 下已全部是 `.py`）。应当改写成 Python，或并入 GN 的单测目标。

## 3. `kernel/arch/*/lib/arch` 没有自己的 `BUILD.gn`

`kernel/arch/{arm,riscv,x86}/lib/arch` 是纯头文件的架构接缝，目前由
`kernel/lib/arch` 统一声明目标。要不要让每个架构各自成为一个可被依赖的目标，
是个设计问题——现在的写法让「依赖 x86 的 arch 接缝」无法单独表达。
