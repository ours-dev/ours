## elfload 拆成 system/lib/elfload 和 kernel/lib/kelfload（U5，2026-09-28）

**文件**
- `system/lib/elfload/`（新）：从 `kernel/lib/elfload` 搬来 `binder.hpp`、`image.hpp`、`program.hpp`、`src/binder.cpp`、`src/program.cpp`、`README.md`；新写 `BUILD.gn`
- `kernel/lib/elfload/` → `kernel/lib/kelfload/`：只剩 Loader（`include/kelfload/loader.hpp`、`src/loader.cpp`），
  命名空间 `elfload::Loader`/`ImageVmo` → `kelfload::Loader`/`ImageVmo`；`BUILD.gn` 重写，`.clang-format` 的第一组改成 `<kelfload/`；新写 `README.md`
- `kernel/lib/userabi/BUILD.gn`、`src/userboot.cpp`：依赖和名字跟着改
- `docs/design/260924/userspace-filesystem.md`：U5 标为已落地

**症状**
K23 的用户态 launcher 要用 Binder 把程序绑到 vDSO，要用 `check_program` 检查程序，可它们在 `kernel/lib/elfload` 里，
和依赖对象层的 Loader 编在同一个库，还依赖内核的 logz4 胶水（串口 logger）。

**原因**
Binder 和 `check_program` 只需要一段内存和 ELF 文件，本来就不依赖内核；只有 Loader 要经对象层把段放进地址空间。
两半放在一个内核库里，用户态就用不了通用的那一半。按分层规则，通用框架放 `system/lib/<name>`，
内核特化放 `kernel/lib/k<name>`（`concurrent` 与 `kconcurrent`）。

**改法**
纯搬家，代码不改，只动位置、命名空间和依赖：

```
kernel/lib/elfload/{binder,image,program}  →  system/lib/elfload/   namespace elfload
kernel/lib/elfload/loader                   →  kernel/lib/kelfload/  namespace kelfload
```

- `system/lib/elfload` 只依赖 `system/lib/eftl`、`system/lib/logz4`（logz4 的接口），不再依赖 `//kernel/lib/logz4`。
  日志照打：谁链接它，谁负责装 logger。内核已经装了串口的；用户态的 logger 等第一个用户态使用者（K23b 的 launcher）出现时由 runtime 装（用户选的方案 A：内核和用户态打出同样的错误信息）。
- `image.hpp`（`elfload::Image`、`ImageWriter`）是两半共享的接口，留在通用那边；Loader 以 `elfload::Image` 作输出。

**参照**
Zircon 的对应拆分：通用的 ELF 装载与重定位在 `src/lib/elfldltl`（内核、userboot、用户态共用），
内核自己的 vDSO/userboot 装载在 `zircon/kernel/lib/userabi`。

**验证**
- 重新 `gn gen` 后两个镜像均构建通过；另用全新的输出目录（`out/u5-fresh.x86`）从零构建一次，排除旧目标文件遮住链接错误，
  `boot_check.py` 两个镜像都 PASS（dbg 14 passed；rel 13 passed、5 skipped）。内核照常装载第一个进程、绑定 vDSO。
- `gn desc out/bringup.x86 //system/lib/elfload deps --all` 里没有任何 `//kernel` 目标。

**故意没做**
- 用户态 logger（runtime 里的 logz4 sink）：没有用户态使用者之前不加，随 K23b 一起。
- elfload 的宿主单测：设计 U5 行写了“宿主单测”，但它和 eftl 的测试一样没有挂进构建图（`docs/repairs/260919/elfload.md` 第 2 条），先把那一条修掉再补。
- 按段规划（`docs/repairs/260919/elfload.md` 第 1 条）不在这一步，第一个使用者是 K23b。

**怎么发现的**
无。

**用户的预测**
无。

**你应该能回答的三个问题**
- 为什么 Binder 能放进 `system/lib`，Loader 却必须留在内核？
- `elfload::Image` 为什么放在通用那一半，而不是跟 Loader 走？
- 一个同时被内核和用户态链接的库，日志该怎么打，为什么不能直接依赖 `//kernel/lib/logz4`？
