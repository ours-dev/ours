# examples —— 改动记录（2026-09-10）

## `//examples/shell`：一个在 ring 3 里列出内核命令注册表的程序（2026-09-10）

**文件**
- `examples/BUILD.gn`、`examples/shell/BUILD.gn`、`examples/shell/shell.cpp` —— 新建
- `system/public/ours/syscall.hpp` —— 新增 `OURS_SYS_COMMAND_LIST`
- `system/public/ours/vdso.hpp` —— 入口表扩到 2 项，版本 1 → 2
- `kernel/lib/userabi/vdso/vdso.S` —— 新表项 + 新桩
- `kernel/lib/syscall/src/command.cpp` —— 新建，`sys_command_list`
- `kernel/lib/syscall/{include/syscall/handlers.hpp,src/table.cpp,BUILD.gn}` —— 接线
- `kernel/products/image/BUILD.gn` —— shell 进 BootFS
- `kernel/lib/userabi/src/userboot.cpp` —— 加载 shell，userboot 作后备

**症状**
不是 bug，是缺口。userboot 证明了内核能造进程、映射 ELF、答一次系统调用，
但它是**内核自己的引导载荷**——证明完就自旋。没有任何"普通用户程序"，
也就没人验证过跨过特权边界之后还能做点什么。

**原因**
挑"列出 `GKTL_COMMAND` 注册表"作为第一件事，是因为它恰好只需要**一个只读、定长、
不接受用户指针进内核回调**的系统调用，却又必须真的跨越边界：名字来自内核链接段，
`examples/shell/shell.cpp` 里一个都没有。

**改法**

vDSO 入口表加一项（每项固定 4 字节偏移，只能往尾部追加——改顺序会让已编译的用户
程序读错位置）：

```c
/* 改前 */
#define OURS_VDSO_TABLE_OFF_DEBUG_WRITE 8
#define OURS_VDSO_TABLE_SIZE 12
#define OURS_VDSO_NR_ENTRIES 1

/* 改后 */
#define OURS_VDSO_TABLE_OFF_DEBUG_WRITE 8
#define OURS_VDSO_TABLE_OFF_COMMAND_LIST 12
#define OURS_VDSO_TABLE_SIZE 16
#define OURS_VDSO_NR_ENTRIES 2
```

`sys_command_list` 走的是 `sys_debug_write` 那种"无对象、直穿"的形状，而不是
dispatcher —— 因为要合规就得先有 handle table，而 `handle_table.hpp` 现在连存储成员
都还注释着。这一点是明说的，不是绕过去的：它只**读**注册表并把文本拷出，
**不调用 `gktl::run_command()`**。让 ring 3 传 `argv` 进内核回调是另一个量级的问题，
留给下一步。

关键的一条安全设计：**内核只往用户缓冲区写，从不从用户那里拿指针进回调**。
范围检查和 `sys_debug_write` 同款（起点不在内核空间、长度不溢出、终点也不在内核空间），
并且**预留一个字节保证结果永远有终止符**——拿到未终止缓冲区的调用者无从知道文本在哪结束。

加载侧，`load_userboot` 泛化成 `load_program(bytes, name, ...)`，`userboot_init` 先找
`shell`，找不到再退回 `userboot`：

```cpp
auto program = PROGRAM_FILE;                       // "shell"
auto image = package.unwrap().file(program);
if (image.is_err()) {
    log::info("userboot: no '{}' in the kernel package, falling back to '{}'", ...);
    program = USERBOOT_FILE;                       // "userboot"
    image = package.unwrap().file(program);
}
```

退回而不是替换，是为了让内核**不依赖 `//examples` 存在**——不带它构建的树照样启动。

shell 本身用的是"没有标准库的现代 C++"：没有 libc、没有 crt1.o、没有动态链接器，
`_start` 就是全部运行时，可用头文件只有 `//system/public` 和整数别名前奏。
所以 `<string_view>` / `<span>` 都够不着，`Str`、`Vdso`、`Console`、`Registry`
是自己写的小词汇类型——但仍然是 constexpr、模板、`enum class`，没有宏：
"没有标准库"不等于"写 C"。

**参照**
Zircon 的 `//zircon/kernel/lib/userabi/userboot` 与 `zx_debug_write`；
`examples/` 与 `kernel/lib/userabi` 的分家对应 Fuchsia 把 userboot 留在内核树、
把普通程序放到 `//examples` 的做法。

**验证**
真机启动（`kernel.dev.dbg_1.obi`，四核），日志里 shell 的完整输出：

```
userboot: found 'shell', 1496 bytes of ELF
ours shell: the kernel's command registry, seen from ring 3
--- commands ---
log  log [level <name>]: dump the record ring, or set the runtime floor
prof.start  prof.start [pmu|tick] [period]: start the sampling profiler
prof.stop  prof.stop: stop the sampling profiler
dump-frame
--- end ---
```

四条名字全部来自内核的 `.gktl.commands` 段，shell 的源码里一个都没有。

**路上撞出并单独修掉的三个真 bug**（各自一个 commit）：
1. `.gktl.commands` 没有 `KEEP`，**整个命令注册表被链接器回收**——列出来是空的才发现。
2. `sys_debug_write` 走 `std::format`，长字符串触发堆分配 → `operator new` panic 桩。
3. 用户工具链没有 `-mno-sse`，而内核从不置 `CR4.OSFXSR` → 第一条 SSE 指令 `#UD`。

**故意没做**
- **没有做输入**。内核侧 kshell 已经有一个线程在轮询 COM1
  （`kshell/shell.cpp:117` 的 `GKTL_INIT_HOOK`），用户态再读就是两个读者抢同一个 UART。
  全树也没有第二条输入路径：i8042 的 `KEY_BUF` 有中断写入但**没有任何读者**，
  `channel_dispatcher` 只有声明没有 `.cpp`。所以"交互"这一步要先决定输入归谁，
  这是下一件事而不是这一件的一部分。
- **没有做 `command_run`**。让 ring 3 命名一个内核命令并传 `argv` 进去，需要把整行
  拷进内核、在内核里切词、传内核自己的 `argv`——而且那些回调是照着"调用者在内核里"
  写的。安全版本要一起设计，不该顺手加。
- **没有走 handle table / dispatcher**。见上：它还不存在。
