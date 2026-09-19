# userabi 待修项（2026-09-19）

## 1. 间歇性：shell 第一次 syscall 调用跳进未映射地址

**状态**：机制已定位，责任方已缩小；**当前树上 20 次启动未复现**，而且出故障的那个
二进制已经不存在了（见「为什么现在复现不了」）。**不能当作已修**。

### 现象

`kernel.dev.obi` 与 `kernel.dev.dbg_1.obi` 各命中过一次：

```
userboot: mapped [200000, 204000) into its own aspace, entry at 201400, stack 1200000
userboot: vdso mapped [A00000, A03000)
Unhandled page fault: vector=14, error_code=0x14
  rip=0x15c6 (+0x77c00015c6)  cs=0x33  rsp=0x11fffa0
  rax=0x0  rbx=0x3b  rcx=0x0  rdx=0x0
  rsi=0x3b  rdi=0x2003b5  rbp=0x11fffc8
  r12=0x0  r13=0x0  r14=0x2003b5  r15=0x3b
```

### 定位到的机制

`error_code=0x14` = P=0（页不存在）+ U/S=1（用户态）+ I/D=1（**取指**），`cs=0x33`
确认 ring 3，`rsp` 在 shell 的栈上（栈顶 `0x1200000`）。

**出错的是 shell 的第一次 `ours_debug_write(msg, 59)`**，证据三条互相印证：

1. `rsi = rbx = r15 = 0x3b` = **59**。正常启动时 shell 打印的第一行是
   `ours shell: the kernel's command registry, seen from ring 3` —— 58 字符 + 换行 = 59。
2. `rdi = r14 = 0x2003b5` 落在 shell 只读段 `[0x200000, 0x2003f1)` 内，正是那条字符串。
3. 正常启动里这一行**总会出现**；出故障那次 `vdso mapped` 之后**什么都没有**就崩了。

这次调用走的是一个**函数指针槽**。反汇编（当时的 shell）：

```
1561: movq 0x1138(%rip), %r12   # 0x26a0     ← 从槽里取
158c: callq *%r12
15b6: jmpq *0x10e4(%rip)        # 0x26a0     ← 尾跳同一个槽
```

而 `0x26a0` 正是一条 `R_X86_64_GLOB_DAT` 重定位的目标：

```
0x26a0  R_X86_64_GLOB_DAT  ours_debug_write + 0
0x26a8  R_X86_64_GLOB_DAT  ours_command_list + 0
```

两个符号在 shell 里都是 UND，**由 `elfload::Binder::bind()` 在装载时填入**。
所以故障 = 第一次通过该槽调用时，槽里的值不是 vDSO 里 `ours_debug_write` 的地址。

`rip=0x15c6` 既不在 shell 的 `[200000,204000)` 里，也不在 vDSO 的 `[A00000,A03000)` 里。

### 已排除

- **不是调度器改动引起的**。带当时那笔调度器改动跑 3 次、回到 HEAD 跑 3 次，六次全干净；
  故障只在最初各一次。
- **不是「线程先于绑定启动」的竞态**。`userboot.cpp` 里 `binder.bind()` 在
  `thread.dispatcher()->start()` **之前**，顺序是对的，注释也写明了这一点。
- **不是绑定算式写错**。`binder.cpp` 里 `value = m_bias + definition + addend`，
  而 `m_bias = image.base - image.image_base`；静态核对：vDSO 导出
  `ours_debug_write` 于 `0x12f0`，装载基址 `0xA00000`，得 `0xA012F0`，正确。

### 为什么现在复现不了

出故障那次日志里 shell 是 **1784 字节**，当前树上是 **1712 字节**——不是同一个二进制。
其间落了 `1af605e`「feat(ou): a named C++ entry for every system call」，新增
`system/lib/ou` 并让 shell 依赖它，shell 因此被重建。

**所以：出故障的那份代码已经不存在了。** 这既可能意味着它被顺带修掉了，也可能意味着它
只是换了个形状还在。20 次启动（6 次对照 + 14 次专门复现）全部正常，shell 输出每次都有。

### 建议的下一步

1. **先把绑定结果变成可见的。** `binder.cpp:198` 是
   `log::debug("elfload: bound '{}' to {:X}", ...)`，而 debug 级别在两个镜像里都没打印——
   所以我手上那两次故障日志里**没有任何绑定值**可查。把它提到 `log::info`（或让 dbg 镜像
   打开 debug），下一次复现就能直接看到槽里被写了什么。这是目前最值钱的一步。
2. 若再次复现，用当次的 shell 二进制把 `rip` 落到符号上——本次做不到，因为二进制已换。
3. 间歇性的来源仍未知。已排除线程启动顺序；还没查的是**绑定写入的可见性**：
   `Binder::bind()` 通过 `importer.view`（内核侧 physmap 视图）写，而用户态从自己的映射读，
   两者是同一批物理页。x86 上数据是相干的，但值得确认 `view` 与用户映射指向的确实是同一批
   帧、且写入发生在映射建立之后没有被别处覆盖。

**归属**：`kernel/lib/userabi`、`kernel/lib/elfload`。
