# `*Control` → `*Options` 收尾（2026-09-19）

## 把剩下的四个子系统里的位掩码也改过来

**文件**

21 个文件，横跨 `kernel/task`、`kernel/mm`、`kernel/arch/x86/mm`、`kernel/lib/arch`：

- `NotifyControl` → `NotifyOptions`（`task/owned_wait_queue.hpp`）
- `MapControl` / `UnmapControl` / `HarvestControl` → `*Options`
  （`lib/arch/include/arch/paging/controls.hpp`）
- `AllocationControl` → `AllocationOptions`（`mm/include/ours/mm/gaf.hpp`）

类型 66 处，形参与函数体内的局部名 15 处。

**症状**

和 `kernel/task/sched` 里已经改过的那三个同病：这些都是**调用方传进来的位掩码**，
说的是「这次操作要按哪些方式做」。「Control」听起来像一个控制器、一个句柄、
某种会去施加影响的东西；它们不是，它们是一组选项。

**改法**

类型改名，形参 `control` 跟着改成 `options`——一个 `MapOptions` 类型的形参叫 `control`
读起来是拧的。函数体内引用该形参的地方一并改。

**一处没改，理由要记下来**

`kernel/lib/arch/include/arch/paging/controls.hpp` **文件名保持不变**。

按理它该叫 `options.hpp`，但那个名字在同一个目录下**已经被占了，而且是完全另一个意思**：
`arch/paging/options.hpp` 装的是 ustl 的模板包选项（`USTL_TYPE_OPTION(PageAllocator, ...)`、
`USTL_CONSTANT_OPTION(PagingLevel, ...)`），是模板元编程的「选项」，不是调用方的标志位。

两个「options」撞在一起会比现在的文件名更糟。我第一遍脚本就是直接 `git mv` 过去，
被「destination exists」挡住才发现——而它在被挡住之前已经把两处 `#include` 改指了
那个不相干的头，已还原。

所以文件名先留着，作为一条待决项：要么给这三个枚举另找一个不撞的文件名，
要么把 ustl 那套模板选项的头改名。两条都超出「把 Control 改成 Options」的范围。

**验证**

全树无 `*Control` 残留。构建成功（按退出码）、无告警。QEMU 两个镜像各 30 秒：
`kernel.dev.dbg_1.obi` 线程交替 169035 次、断言/panic/异常计数 0；
`kernel.dev.obi` userboot 正常、计数 0。
