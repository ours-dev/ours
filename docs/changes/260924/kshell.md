# kernel/lib/kshell — 改动记录

## 构建期开关 kernel_shell_enabled：可以把 kshell 整个编出镜像（2026-09-24）

**文件**
- `kernel/kernel_params.gni` —— 新增构建参数 `kernel_shell_enabled`，默认 `true`
- `kernel/top/BUILD.gn` —— `//kernel/lib/kshell` 和 `//kernel/commands` 两条依赖改为受它控制
- `kernel/commands/BUILD.gn` —— 顶部注释指向这个开关
- `kernel/lib/kshell/README.md` —— 新增「Turning it off」一节
- `tools/set.py` —— 新增 `--args NAME=VALUE`，把任意 GN 参数写进 args.gn

**症状**
潜伏，不是崩溃：**kshell 根本关不掉**。只要是 vmours，就一定带着它，一定会起一个
ring 0 线程轮询 COM1，读到一行就交给 `gktl::run_command()`。能往串口写字的人就能跑
每一条 `GKTL_COMMAND`——`prof.*`、`log level`、`mutex.stress`（三个 CPU 抢一把锁
两万轮）。ring 3 只能 *列出* 命令（`sys_command_list`），但串口这一侧是全权的。

**原因**
依赖是无条件写死的：

```gn
# kernel/top/BUILD.gn（改前）
deps = [
  ...
  "//kernel/commands",
  ...
  "//kernel/lib/kshell",
  ...
]
```

而 `kshell/shell.cpp` 里 `GKTL_INIT_HOOK(KShellInit, spawn, InitLevel::Platform)`
一旦被链接进来就一定会跑。所以「这个内核有没有调试后门」不是任何配置能回答的问题，
只取决于它是不是 vmours。

`//kernel/commands/BUILD.gn` 开头早就写了「让不带控制台的内核不必背着这些回调和帮助
字符串」——设计上预见了「不带控制台的内核」，但构建里从来没有能造出它的开关。

**改法**

```gn
# kernel/kernel_params.gni
kernel_shell_enabled = true
```

```gn
# kernel/top/BUILD.gn（改后）
deps = [ ...不再含 commands / kshell... ]
if (kernel_shell_enabled) {
  deps += [
    "//kernel/commands",
    "//kernel/lib/kshell",
  ]
}
```

几个取舍：

1. **切依赖边，不切代码**。这个仓库里宏只负责把构建期配置值带进来，常量用
   constexpr；在 `shell.cpp` 外面套 `#if KERNEL_SHELL` 就是拿宏包住一整段代码。
   而且没必要：两个库都没有哪一行在「没有 shell」时还有意义，整库进或整库出正好。
   照的是树里已有的先例——`sched_normal_discipline`（`kernel/kernel/task/sched/BUILD.gn`）
   和 `mm_vflat_memmap`（`kernel/mm/BUILD.gn`）都是 `kernel_params.gni` 里的参数在
   BUILD.gn 里选源文件。

2. **commands 跟着 kshell 一起走**。`//kernel/commands` 存在的唯一目的就是往 kshell
   提示符上放命令；没有 shell，它就是没人能调的回调加没人会打印的帮助字符串。

3. **注册表本身留下**。gktl 不等于控制台，它还承载 init hook 和计数器；长在子系统自己
   文件里的命令（mm 的 `dump-frame`，`kernel/mm/pm_frame.cpp`）照样把记录放进
   `.gktl.commands`，`sys_command_list` 照样列得出来。这不是泄漏：那个系统调用只列不跑，
   而 shell 一走，`gktl::run_command()` 就没有调用者了，`--gc-sections` 把它回收掉
   ——实测下面的符号表里它确实没了——于是**没有任何路径能执行任何命令**。
   Zircon 在这里的做法不同：`STATIC_COMMAND` 在 `CONSOLE_ENABLED` 为假时被
   `#if` 整个编掉，连记录都没有。我们不用宏去做，代价是多留一条几十字节的记录。

4. **选择机制用 args.gn，并给 `tools/set.py` 加 `--args`**。`set.py` 每次都从头重写
   args.gn（文件头就写着 do not edit by hand），手工加进去或用 `gn args` 加的参数下次
   一跑 set.py 就没了。所以一个构建参数要真正「可选」，就得能在 set.py 上说出来，
   跟 `fx set --args` 一样：

   ```
   tools/set.py bringup.x86 --out out/bringup.x86.noshell --args kernel_shell_enabled=false
   ```

**参照**
- Zircon 把控制台挂在 switchset 上：`zircon/kernel/kernel_switchset.gni` 的表格里
  `lk_debug_level` 0 级 Console = N，实现在 `zircon/kernel/switch/BUILD.gn` 的
  `variant("lk_debug_level_${level}")`（`level > 0` 才加 `--cfg=console_enabled`）
  和 `zircon/kernel/lib/console/include/lib/console.h` 的
  `#define CONSOLE_ENABLED (LK_DEBUGLEVEL > 0)`。
- 但 `//zircon/kernel/top:main` 仍然无条件依赖 `//zircon/kernel/lib/console`
  （`zircon/kernel/top/BUILD.gn`），关的是宏展开出来的东西，不是依赖边。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86`（默认，开）和
  `prebuilt/ninja/ninja -C out/bringup.x86.noshell`（关）都通过。
- `llvm-nm -C` 对比（数的是 kshell / 各命令回调 / `run_command` / `dump_frame`）：

  | 镜像 | 开 | 关 |
  |---|---|---|
  | dbg.0 `vmours.with-tests` | 8 个符号，15 580 704 B | 只剩 `ours::mm::cmd_dump_frame`，15 386 320 B |
  | dbg.1 `vmours` | 8 个符号，31 428 072 B | 只剩 `ours::mm::cmd_dump_frame`，30 737 568 B |

  关掉以后 `kshell::spawn`、`GKTL_INIT_HOOK_KShellInit`、`log_command`、
  `prof_*_command`、`mutex_stress_command`、`gktl::run_command` 全部不在。
- QEMU（`scripts/qemu_boot.py`）：
  - 默认 dev：`init: shell says "pong"`、`shell: channel checks passed`、
    `shell: bad-pointer checks passed` 都在；`kshell> ` 下敲 `mutex.stress` 得到
    `mutex.stress: ok -- 20000 rounds x 3 threads`。
  - 关掉的 dev：上面三行都在，**没有 `kshell> `**；ring 3 shell 列出的命令只剩
    `dump-frame`。
  - 关掉的 dbg_1：`channel checks passed` / `bad-pointer checks passed` 在，命令表只剩
    `dump-frame`。（dbg_1 本来就从不出现 `kshell> `，改动之前就是这样，不是本改动引起的。）

**故意没做**
- **没有像 Zircon 那样把它绑到 debug level / switchset 上**。dev 镜像用的是 dbg.0，
  照 Zircon 的表 dbg.0 就没有控制台，而 dev 正是靠串口 shell 被自动化驱动的
  （`prof.*`、`mutex.stress`）。Zircon 自己在那张表下面也留了
  `TODO(fxbug.dev/42117912): Break this up into individual knobs`。
  如果以后需要「某个 switchset 必定不带 shell」，在 `kernel_switchset` 里像
  `kernel_unit_tests_enabled` 那样经 `toolchain_args` 传下去即可，现在没有这样的
  switchset，就不预先加。
- **没有把 `dump-frame` 挪进 `//kernel/commands`**。`kernel/commands/BUILD.gn` 明确说
  子系统自己文件里的命令留在原处；而且它现在是个空壳（`return 0`），可能是留给后续的
  脚手架。
- **没有让 `sys_command_list` 在无 shell 时返回空**。它列的是注册表，不是 shell 能跑
  什么；让它知道 shell 存不存在，就得把构建参数再接进 syscall 库，换来的只是少列一行。
