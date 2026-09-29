## eftl 从 kernel/lib 搬到 system/lib（2026-09-28）

**文件**
- `kernel/lib/eftl/` → `system/lib/eftl/`（`git mv`，整个目录，含 `test/`）
- 引用它的地方：`kernel/phys/BUILD.gn`、`kernel/lib/elfload/BUILD.gn`、`kernel/lib/elfload/src/loader.cpp` 与
  `kernel/phys/tests/build_id_note_smoke.cpp` 的注释、`system/lib/eftl/README.md`、`system/lib/eftl/test/BUILD.gn`
- `docs/design/260924/userspace-filesystem.md`、`docs/repairs/260919/elfload.md`：路径跟着改

**症状**
U5 要把 Binder 挪到 `system/lib/elfload`，给用户态 launcher 用；Binder 依赖 eftl，而 eftl 在 `kernel/lib` 下。
不挪的话，`system/lib` 就要反过来依赖 `kernel/lib`。

**原因**
eftl 是纯头文件的通用 ELF 库，只依赖 `ustl` 和 `system/public`，没有一行内核代码；
它的 README 自己就写着“for use in kernels, linkers, loader tools”。按分层规则（ours-kernel-design 的 layering：
按依赖决定位置，而不是按离调用者近），它从一开始就该在 `system/lib`。
放在 `kernel/lib` 大概只是因为它的第一个使用者是 phys。

**改法**
纯搬家，内容不变：`git mv`，然后把所有 `//kernel/lib/eftl` 改成 `//system/lib/eftl`，注释和文档里的路径一并改。

**参照**
Zircon 同类库的位置：通用的 ELF 解析在 `src/lib/elfldltl`（用户态、内核、phys 共用），不在 `zircon/kernel` 下。

**验证**
- `gn gen` 后 `out/bringup.x86` 两个镜像构建通过；`boot_check.py` 两个镜像都 PASS（dbg 14 passed；rel 13 passed、5 skipped）。
- `out/phys-host-tests`、`out/mm-host-tests` 重新 `gn gen` 通过，`out/phys-host-tests` 全量构建通过。

**故意没做**
- 没有把 `system/lib/eftl:tests` 挂进 `system/lib:tests` 汇总组：那是 `docs/repairs/260919/elfload.md` 第 2 条，
  挪到 system/lib 之后只差这一行，但挂上之后可能暴露测试本身的构建问题，单独做。

**怎么发现的**
准备 U5 时查 Binder 的依赖。

**用户的预测**
无。

**你应该能回答的三个问题**
- 一个库该放 `system/lib` 还是 `kernel/lib`，用什么标准判断？
- 如果 eftl 留在 `kernel/lib`，用户态 launcher 依赖它会出什么问题？
- `system/lib/<name>` 和 `kernel/lib/k<name>` 的分工是什么？
