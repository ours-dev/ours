# 让下一次故障能自己说明问题（2026-09-19）

## 缺页转储补上 CR2

**文件**
- `kernel/arch/x86/kernel/faults.cpp` — `panic_exception()` 增加一行

**症状**

`Unhandled page fault` 的寄存器转储里**没有故障地址**。它打了 `rip`、通用寄存器、
`error_code`，唯独没有 CR2——也就是那条指令到底在够哪个地址。

这不是假设的缺憾。同日排查一次 ring 3 取指缺页时（见
`docs/repairs/260919/userabi-user-fault.md`），手上只有 `rip` 和通用寄存器；
那一次因为是**取指**故障、CR2 恰好等于 `rip`，所以没吃亏。任何一次**数据**故障都会吃亏：
读者只能从 `rip` 反推那条指令在算什么地址。

**改法**

只在 `vector == PageFault` 时打印——CR2 只对缺页有意义。读它是安全的：从进入这个函数到
打印之间不会再发生缺页（下面没有任何会触发故障的代码，函数以 halt 循环收尾），
所以 CR2 仍是那次故障的值。

**验证**

不是「编过了就算」。临时在 `labour_routine()` 里植入一次故意的写：

```cpp
*reinterpret_cast<volatile u64 *>(0xDEADBEEF000) = 1;
```

启动后转储打出：

```
Unhandled page fault: vector=14, error_code=0x2
  cr2=0xdeadbeef000
  rip=0xffffff8840009901 (+0x9901)  cs=0x10  ...
```

`cr2` 正是写入的地址。探针随即撤除，`git diff` 确认 `main.cpp` 已还原。

## elfload 把绑定结果打到 info

**文件**
- `kernel/lib/elfload/src/binder.cpp` — 一行日志级别

**症状**

`Binder::bind()` 把每条解析结果记在 `log::debug`，而 debug 在**两个镜像里都低于输出下限**。
于是那次 ring 3 故障的两份日志里，**一个绑定值都没有**——而故障恰恰就是「某个被绑定的槽
里值不对」。唯一的见证者是一份没有符号的寄存器转储。

**改法**

提到 `log::info`。一个程序就几行，而它正是下一次这种转储旁边最需要的东西。

**验证**

```
userboot: vdso mapped [A00000, A03000)
elfload: bound 'ours_debug_write' to A012F0
elfload: bound 'ours_command_list' to A01300
```

两个值与静态核对一致（vDSO 导出 `0x12f0` / `0x1300`，装载基址 `0xA00000`），
也就是**当前的绑定是对的**。两个镜像启动正常，断言/panic 计数 0。
