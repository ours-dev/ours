# x86/kernel 待修项（2026-09-23）

对 P0 系统调用系列做对抗式审查时发现的，不是那个系列引入的。（原第 2 条"vDSO 入口校验的下边界差一个指令长度"已由 P1-A 第 28 条修掉，见 `docs/changes/260924/syscall.md`。原第 1 条已分两步修掉：IST 见 `docs/changes/260925/x86-kernel.md` 第 1 条，swapgs 窗口里的 %gs 见同文件第 2 条。本文件已无待修项；之后发现的记在 `docs/repairs/260925/x86-kernel.md`。）
