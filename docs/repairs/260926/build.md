# build 待修项（2026-09-26）

## 1. dbg_1 没有 Zircon 调试镜像那种压力测试启动开关，因为内核侧还没有读它们的代码

**现象**：潜伏的缺口，不是故障。`kernel.dev.dbg_1` 与 `kernel.dev` 的区别只在内核按
`kernel_debug_level=1` 编译（`DEBUG_ASSERT` 打开），cmdline 上没有任何逼出错误路径的开关。

**原因**：Zircon 的调试镜像除了换 debug-level switchset，还在 cmdline 上加"让内核故意更难受"
的开关，例如 `zircon/kernel/image/eng/BUILD.gn` 的 `test-boot-options` 里的
`kernel.pmm.alloc-random-should-wait=true`（PMM 分配随机等待，逼出没处理好分配延迟/失败的代码）和
`kernel.compression.random-debug-compress=true`。这些开关只有在内核代码里读它们时才起作用。
ours 的 `kernel/include/ours/boot/options.hpp` 目前只认 `kernel.select.userboot`、`kernel.shell`、
`kernel.name`，所以只在 BUILD.gn 里加这几行 cmdline 是空操作——开关没有接到任何东西上。

**修法方向**：先在内核侧实现开关（`BootOptions` 加字段，PMM 等处读取并改变行为，
对照 Zircon `lib/boot-options/options.inc` 与 PMM 里 `alloc-random-should-wait` 的用法），
再在 `kernel/products/image/dev.dbg_1/BUILD.gn` 的 `kernel_cmdline("cmdline")` 里打开。
这是内核侧的活，不是构建能单独完成的。
