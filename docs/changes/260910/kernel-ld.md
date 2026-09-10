# kernel-ld —— 改动记录（2026-09-10）

## `.gktl.commands` 没有 KEEP，整个 kshell 命令注册表被链接器回收了（2026-09-10）

**文件**
- `kernel/kernel.ld` —— `.gktl.commands*` 加上 `KEEP()`

**症状**
**`GKTL_COMMAND` 注册的命令，一条都不在最终镜像里。**

```
$ llvm-nm vmours | grep '__commands_'
000000000005e000 r __commands_end
000000000005e000 r __commands_start
```

起止相同 —— 区间长度 0，记录数 0。而目标文件里它们都在：

```
$ llvm-readelf -S commands.log.cpp.o | grep gktl.commands
.gktl.commands
```

也就是说 `kshell` 在串口上敲 `log`、`prof.start`、`dump-frame`，
`gktl::run_command()` 的 radix 树是空的，永远找不到，**而且不出声**——
`command.cpp:87` 找不到就 `return`，未知命令和被回收的命令表现完全一样。

**怎么发现的**
写 `//examples/shell` 时加了一个 `sys_command_list` syscall 让用户态列出注册表，
结果列出来是空的。先怀疑 syscall 写错了，一查 `__commands_start == __commands_end`。

**原因**
一条 `GKTL_COMMAND` 记录**没有任何引用**——「被这段区间扫到」就是它全部的存在方式：

```cpp
#define GKTL_COMMAND(NAME, CMD, HANDLER, HELP)                          \
    namespace commands::NAME {                                          \
        LINK_SECTION(".gktl.commands")                                  \
        FORCE_USED static ::gktl::Command const COMMAND_##NAME{...};    \
    }
```

`FORCE_USED`（`__attribute__((used))`）拦得住**编译器**，拦不住**链接器**。
内核用 `-fdata-sections` + `--gc-sections` 构建，于是每条记录各自成节、无人引用、
全部被回收。

而这个文件里**其它每一个同类注册表节都写了 `KEEP`**：

```
KEEP(*(SORT_BY_ALIGNMENT(.hsien.tests)))
KEEP(*(SORT_BY_ALIGNMENT(.gktl.init_hook*)))
KEEP(*(SORT_BY_ALIGNMENT(.gktl.counter.descs)))
KEEP(*(SORT_BY_ALIGNMENT(.task.schedules)))
```

只有命令这一条漏了。

**改法**

```
/* 改前 */
        PROVIDE_HIDDEN(__commands_start = .);
        *(.gktl.commands*)
        PROVIDE_HIDDEN(__commands_end = .);

/* 改后 */
        PROVIDE_HIDDEN(__commands_start = .);
        /* KEEP, like every other registry section in this file. ... */
        KEEP(*(SORT_BY_ALIGNMENT(.gktl.commands*)))
        PROVIDE_HIDDEN(__commands_end = .);
```

`SORT_BY_ALIGNMENT` 跟着同文件其它注册表的写法，理由也一样：记录之间没有顺序要求，
按对齐排能少几个填充洞。

**参照**
Linux 的 `KEEP(*(__param))` / `KEEP(*(___kentry+*))`，以及 Zircon 链接脚本里
`KEEP(*(.data.rel.ro.lk_init))` —— 「靠区间扫描找到」的表都必须 KEEP，是同一个道理。

**验证**
```
改前： start=0x5e000 end=0x5e000 bytes=0   records=0
改后： start=0x5f000 end=0x5f078 bytes=120 records=5
```
5 条，与全树 `GKTL_COMMAND` 的数量一致（log、prof.start、prof.stop、prof.dump、
dump-frame）。用户态 shell 现在能把它们列出来：

```
log  log [level <name>]: dump the record ring, or set the runtime floor
prof.start  prof.start [pmu|tick] [period]: start the sampling profiler
prof.stop  prof.stop: stop the sampling profiler
dump-frame
```

**故意没做**
- **没有给注册表加"至少 N 条"的构建期断言**。能防住这类回归，但要选一个数字，而这个
  数字每加一条命令就要改。真正的防线是这条记录本身——以及现在有一个会去读它的用户程序。
