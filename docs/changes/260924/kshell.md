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

## 启动参数 kernel.shell：默认不起 shell，要用的镜像自己打开（2026-09-24）

**文件**
- `kernel/include/ours/boot/options.hpp` —— `BootOptions` 加 `SHELL_OPTION` 和 `shell()`，默认 `false`
- `kernel/top/boot_options.cpp` —— 新增 `parse_flag()`，解析 `kernel.shell`
- `kernel/lib/obi/include/obi/command_line_view.hpp` —— 新增 `CommandLineView::parse_bool()`
- `kernel/phys/boot_options.cpp` —— 删掉自己的 `parse_bool()`，改用上面那个
- `kernel/lib/kshell/kshell/shell.cpp` —— `spawn()` 在 `kernel.shell` 关着时直接返回并打一行日志
- `kernel/lib/kshell/BUILD.gn` —— 加 `//kernel/phys:handoff`（obi 头文件）
- `kernel/products/image/dev/BUILD.gn`、`kernel/products/image/dev.dbg_1/BUILD.gn` —— cmdline 加 `kernel.shell=true`
- `kernel/lib/kshell/README.md` —— 「Turning it on and off」

**症状**
潜伏，同上一条：链接进来的 kshell 一定会启动。上一条让它可以不被链接，但一个带着
shell 的内核（比如同一份 vmours 既给调试镜像也给别的镜像用）仍然没有办法「带着但
不开」。

**原因**
`KShellInit` 这个 init hook 无条件 `spawn()`：

```cpp
// kernel/lib/kshell/kshell/shell.cpp（改前）
auto spawn() -> void {
    auto *thread = ours::task::Thread::spawn("kshell", ...);
    ...
    thread->resume();
}
```

「要不要起 shell」本该是被启动的那个镜像的决定，却被写死成了「起」。更要紧的是
默认值的方向：shell 是一个在 ring 0 里、谁往串口写字就替谁跑命令的线程，这种东西
应该是**要了才有**，而不是**忘了关就有**。Zircon 的 `kernel.shell` 默认就是 `false`。

**改法**

```cpp
// kernel/lib/kshell/kshell/shell.cpp（改后）
auto spawn() -> void {
    if (!ours::boot::options().shell()) {
        log::info("kshell: not started ({} is off)", ours::boot::BootOptions::SHELL_OPTION);
        return;
    }
    ...
}
```

```gn
# kernel/products/image/dev/BUILD.gn（改后）
kernel_cmdline("cmdline") {
  args = [
    "kernel.select.userboot=init",
    "kernel.shell=true",
  ]
}
```

几个取舍：

1. **读参数走上一步做好的 `boot::BootOptions`，不在 kshell 里再查一次命令行**。
   这正是那一步的目的：名字、默认值、语法只写一处。

2. **布尔语法和 phys 共用一份**。phys 读 `aslr.disabled` 用的是自己匿名命名空间里的
   `parse_bool()`（`1/true/yes`、`0/false/no`，其他值报错）。内核第一次有布尔参数，
   如果再写一份，同一条命令行上的同一个词就可能对两个读者意思不同。所以把它提到
   两者都能用的 `obi::CommandLineView::parse_bool()`，phys 改为调用它，行为不变：

   ```cpp
   // kernel/phys/boot_options.cpp（改前）
   if (!parse_bool(value.value(), m_aslr_disabled)) { return Status::InvArgs; }
   // （改后）
   auto const parsed = obi::CommandLineView::parse_bool(value.value());
   if (parsed.is_none()) { return Status::InvArgs; }
   m_aslr_disabled = parsed.value();
   ```

   和 Zircon 的差别是有意的：Zircon `BootOptions::Parse(bool)` 把除
   `false`/`0`/`off` 之外的一切（包括拼错的词）都当成 true；这里拼错的值会报错并保留
   默认值。对一个默认关的后门来说，`kernel.shell=flase` 应该是一句抱怨，而不是一个 shell。

3. **在 hook 里判断，不去「不注册 hook」**。注册是链接期的段记录，没法依赖命令行；
   「根本不链接」是构建的决定（上一条的 `kernel_shell_enabled`），不是启动的决定。
   两层是正交的：构建关了，启动参数写什么都没用（读了，没人消费）。

4. **关着的时候打一行 info**。上一次 kshell 不出现（线程创建了没 resume，
   `docs/changes/260910/kshell.md`）日志里什么都没有，「没有提示符」和「没有 shell」
   看起来一样。现在关着会明说 `kshell: not started (kernel.shell is off)`。

5. **打开放在镜像 cmdline 里，不放产品级 `kernel_cmdline`**，照 `kernel.select.userboot`
   的先例：跑什么、开不开 shell 都是镜像的属性。

**参照**
- Zircon `lib/boot-options/include/lib/boot-options/options.inc` 的
  `DEFINE_OPTION("kernel.shell", bool, shell, {false}, ...)`。
- Zircon `kernel/lib/console/mod.rs` 的 `kernel_shell_init()`：
  `if boot_options.shell { console_start(); }`——同样的检查，同样在「要启动时」做。
  它由 `kernel/top/main.cc` 在 `userboot_init()` 之前调用。
- Zircon `BootOptions::Parse(std::string_view, bool BootOptions::*)`：布尔语法，见上。

**验证**
- 共享 worktree 里另一个会话有未提交的改动（先是 logz4，后是 `kernel/commands/log.cpp`）
  让整棵树暂时编不过，所以这一条在「HEAD + 本改动」的临时 worktree 里构建和启动；
  提交的文件和那里测的逐字节一致。下表在 16ede780 上测；logz4 的两个提交落地后，
  又在 11c4ae93 上重测了 dev 开（提示符 + `mutex.stress`）、dev 关、dbg_1 开，结果相同。
- 构建通过；clang-format 无改动；clang-tidy 对改动行无新告警（phys 头里的
  `BOOT_OPTIONS`、obi 的 `text_` 是原有的）。
- QEMU，逐一改 cmdline 验证四种写法：

  | 镜像 cmdline | 结果 |
  |---|---|
  | dev `kernel.shell=true` | `kshell> ` 出现，敲 `mutex.stress` → `ok -- 20000 rounds x 3 threads` |
  | dev 不写 | `kshell: not started (kernel.shell is off)`，无提示符，敲 `log` 无回应 |
  | dev 裸 `kernel.shell` | `kshell> ` 出现，`log` 正常 |
  | dbg_1 `kernel.shell=flase` | `boot: kernel.shell=flase is not a boolean, keeping false`，未启动 |
  | dbg_1 `kernel.shell=false` | `kshell: not started (kernel.shell is off)` |
  | dbg_1 `kernel.shell=true` | 无「not started」行（选项读到了）；这次没出提示符——dbg_1 的提示符本来就时有时无（上一步重构的那次启动里出现过），与本条无关 |

  所有 dev 启动都有 `init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed`；所有 dbg_1 都有两条 checks passed。
- 两层叠加：`kernel_shell_enabled=false` 构建 + `kernel.shell=true` 镜像 → 符号表里
  没有 kshell / `run_command` / 命令回调，启动日志里没有任何 kshell 行，pong 和两条
  checks 都在，ring 3 列出的命令只剩 `dump-frame`。

**故意没做**
- **`kernel.shell.script` 没接**。`kernel/kernel_shell_script.gni` 已经按 Zircon 的编码
  产出它，但没人读；接上它要在 kshell 里加「先跑脚本、再决定是否进交互」的逻辑，
  不是这一条的范围。
- **没有让 `sys_command_list` 看 `kernel.shell`**。原因同上一条：它列注册表，不执行。
- **dbg_1 不出提示符的问题没查**：改动之前就是这样，与本条无关。

## kshell 改用具名日志目标 `(kshell)`，派发命令时打一行 debug（2026-09-24）

**文件**
- `kernel/lib/kshell/kshell/shell.cpp` — 声明 `KSHELL` 目标；两处手写 `"kshell: "` 前缀的日志改走它；启动成功打一行 info；`dispatch()` 打一行 debug

**症状**
kshell 的日志靠手写前缀区分出处（`log::warn("kshell: failed to spawn console thread")`），
而且正常启动时 kshell 一行日志都不打——提示符是直接写 COM1 的，不经过 logger，
所以记录里看不出控制台到底起没起来。敲了命令没反应时，也分不清是命令没被派发，
还是派发了但命令本身什么都没说：`gktl::run_command()` 对不认识的名字一声不吭。

**原因**
没有具名目标之前，"只看 kshell 的调试输出"只能把全局下限放到 debug，
和 `kernel/top/main.cpp` 测试线程每秒上万行的 `I'm Thread N` 混在一起，等于没有。
所以 kshell 里也就没人写 debug 行。

**改法**
kshell 是 logz4 目标（`docs/changes/260924/logz4.md`）的第一个真正用户：

```cpp
// shell.cpp（改前）
log::info("kshell: not started ({} is off)", ours::boot::BootOptions::SHELL_OPTION);
log::warn("kshell: failed to spawn console thread");

// shell.cpp（改后）
constexpr auto &KSHELL = logz4::Target::named<"kshell">;
...
KSHELL.info("not started ({} is off)", ours::boot::BootOptions::SHELL_OPTION);
KSHELL.warn("failed to spawn console thread");
KSHELL.info("console on COM1");                                   // 新增，resume() 之后
KSHELL.debug("run '{}' with {} argument(s)", argv[0], argc - 1);  // 新增，dispatch() 里
```

每条命令一行，用 debug 而不是 info：平时在提示符下是噪声，要查"命令为什么没反应"时
`log level kshell debug` 单独打开，其它子系统的 debug 不受影响。

**参照**
- Linux：文件顶部 `#define pr_fmt(fmt) "kshell: " fmt`，`pr_debug()` 由 dynamic_debug 按模块打开。
- Zircon 的内核 shell（`kernel/lib/console`）没有等价的派发日志。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format / clang-tidy 对 shell.cpp 无告警。
- QEMU `kernel.dev.obi`：`[... info] (kshell) console on COM1`，以及
  `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`。
- QEMU `kernel.dev.dbg_1.obi`，在 `kshell> ` 依次敲：

  ```
  log level warn          → 全局放到 warn，main.cpp 测试线程的 debug 洪流停下
  log targets             → log: global floor warn / kshell  warn (default)
                            （这条命令自己的 debug 行被全局 warn 挡住）
  log level kshell debug
  log targets             → [... debug] (kshell) run 'log' with 1 argument(s)
                            log: global floor warn / kshell  debug
  log dump kshell         → 只回放 (kshell) 的记录
  log level kshell default
  log targets             → 无 debug 行，kshell  warn (default)
  ```

  打开 kshell 的 debug 之后，日志里只有 `(kshell)` 的 debug 行，`I'm Thread`、`trace` 行数为 0。
  `log dump kshell` 里没有启动时那行 `console on COM1`：256 槽的环在这之前已经被测试线程的
  debug 行冲掉了，符合环的语义。

**故意没做**
- **提示符和回显仍然直接写 COM1，不走 logger。** 它们是终端交互，不是日志；
  代价是提示符会和并发的日志行交错（实测 `kshel[4255394000 cpu0 info] ...`），这是原有行为。
- **没有替 `gktl::run_command()` 报"未知命令"。** 它返回 void，要报得改 gktl 的接口，
  不属于这一条。
