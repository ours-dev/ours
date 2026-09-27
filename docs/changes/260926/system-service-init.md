## init 在起完 shell 后运行 `ours.autorun.boot` 指名的程序（2026-09-26）

**文件**
- `system/service/init/init.cpp` — 新增 `AUTORUN_OPTION`、`find_autorun()`、`start_autorun()`；`main()` 写完 ping、开始等待 shell 之前启动 autorun 程序

**症状**
`tools/set.py --with //foo` 只会把 foo 打进 BootFS，开机不会运行它（`69526fd2`）。并行开发的会话要想「开机就跑自己的程序」，只能去改镜像的开机流程。

**原因**
Fuchsia 的模型是：开机流程属于镜像，构建只负责让更多程序「可用」；「开机时额外跑一条命令」是内核 cmdline 选项 `zircon.autorun.boot` 的事，由 console-launcher（起 shell 的那个组件）执行。ours 里起 shell 的是 init，所以这件事该由 init 来做。前两条改动（`docs/changes/260926/userabi.md` 最后两节）已经让这个选项经内核、userboot 到达 init 的 environ，但 init 还没有读它。

**改法**
改前（`main()`，写完 ping 就开始等 shell 回答）：
```cpp
    if (auto const status = ou::channel_write(here, PING.data(), static_cast<u32>(PING.size()));
        status != Status::Ok) {
        report("init: write(ping) = ", status);
        halt();
    }

    // Asleep until the shell writes back, or its end of the channel goes.
```
改后：
```cpp
    if (auto const autorun = find_autorun(); !autorun.empty()) {
        static_cast<void>(ou::debug_line({"init: autorun ", to_str(autorun)}));
        if (auto const status = start_autorun(job, autorun); status != Status::Ok) {
            report("init: starting the autorun command = ", status);
        }
    }

    // Asleep until the shell writes back, or its end of the channel goes.
```
- 语法 `ours.autorun.boot=<name>[+arg...]`：`<name>` 是 BootFS 文件名，同时也是 argv[0]；`+` 是参数分隔符，因为内核 cmdline 的一个词里不能有空格。空词会被跳过（`a++b`、末尾的 `+`），和 Fuchsia 的 `kSplitWantNonEmpty` 一致。
- 出现多次时，以最后一个为准，和 userboot 的 `find_next()` 同理：后面的词覆盖前面的，这样 bootloader 才能在镜像默认值之后追加。
- 超过 8 个词直接拒绝（`OutOfRange`），不截断：截断后的 argv 跑的是一条没人写过的命令。
- 放在 ping 之后、等待之前：shell 可能永远不放手它那端的通道，autorun 不该等这个。Zircon 也是在起串口 shell 的同时启动 autorun。
- 失败只报错。坏的是这条命令，不是系统，而此时 shell 已经在跑了。
- 被启动的程序只拿到 argv：不给 job，因为它目前能被要求做的事都不需要起进程；也不给 BootFS，这个 init 自己留着。

被否掉的方案：像早期 Fuchsia 那样把 `+` 换成空格，交给 shell 去 `sh -c`。ours 的 shell 还不能执行命令行，而且那样一来，是否能 autorun 就绑在了 shell 的能力上。

**参照**
- `Fuchsia console-launcher main.cc` 的 `LaunchAutorun()`：按 `+` 拆分（`fxl::SplitStringCopy(..., kSplitWantNonEmpty)`），把得到的词直接作为 argv 启动，在 `RunSerialConsole()` 之前调用。`console_launcher.cc` 的 `GetArguments()` 读 `autorun_boot`；`meta/console-launcher.cml` 的 `fuchsia.zircon.autorun.boot`；`docs/reference/kernel/kernel_cmdline.md` 的 `zircon.autorun.boot`。
- 和 Zircon 的差别：Zircon 要求绝对路径（程序在命名空间里），这里是 BootFS 名字；Zircon 等 `/dev` 挂好再跑，这里没有要等的东西。

**验证**
另开 `out/autorun`（`--with //kernel/lib/syscall/test/fault_probe`），每次构建时临时往 dev 的 cmdline 里加选项，构建完立即还原 `BUILD.gn`：
- 构建通过；clang-tidy 在 `init.cpp` 上没有诊断。
- `ours.autorun.boot=fault_probe+none++extra+`：`init: autorun fault_probe+none++extra+` → `fault_probe: none` → `fault_probe: unknown probe none` → `fault_probe: no answer channel (User0)`，进程随后被销毁；shell 照常 `init: shell says "pong"`。
- `ours.autorun.boot=nosuch+x`：`userabi: 'nosuch' is not in the kernel package: NotFound`、`init: starting the autorun command = NotFound`，shell 照常回答 pong。
- 不加选项：日志和改动前一致，没有 `autorun` 字样。
- dbg_1（跑了 53 秒）：shell 的各组检查全部通过。这个镜像直接起 shell、不经过 init，行为不受影响。

**故意没做**
- `ours.autorun.system`：还没有 `/system` 这个概念。选项名里保留 `.boot` 后缀，就是给它留的位置。
- 等 autorun 程序结束或收集它的退出码：需要进程句柄上的 TERMINATED（设计 K23）。
- `tools/set.py --autorun` 开关，以及 gn gen 之后检查「autorun 指名的程序必须在包里」：这两件归构建会话。

## autorun 的 `+` 拆分抽成 `init::AutorunCommand`，加主机单测（2026-09-26）

**文件**
- `system/service/init/include/init/autorun.hpp` — 新增：`AUTORUN_OPTION`，以及 `AutorunCommand::parse()` / `name()` / `words()`
- `system/service/init/init.cpp` — `start_autorun()` 改为调用 `AutorunCommand::parse()`，本地的拆分循环、`MAX_AUTORUN_WORDS`、`AUTORUN_OPTION` 都移走
- `system/service/init/BUILD.gn` — 新增 `cpp_interface("autorun")`；`user_executable("init")` 包进 `if (!is_host_toolchain)`
- `system/service/init/test/BUILD.gn`、`test/autorun_test.cpp` — 新增主机单测
- `BUILD.gn` — `//:default` 加上 `//system/service/init/test:autorun($host_toolchain)`

**症状**
上一条改动只在 QEMU 里验证过 argv[1]：fault_probe 只打印 argv[1]，所以「跳过空词」「后面的词成为 argv[2]」「超过 8 个词拒绝」这几条行为，没有任何地方能看到。

**原因**
拆分写在 `start_autorun()` 里，和建通道、起进程搅在一起，而 init 只存在于 user 工具链，想在主机上跑它的代码是做不到的。要验证拆分，就只能每种写法都开一次机，而且还得有一个会把 argv 全部打印出来的程序。可拆分本身只是字符串处理，不需要内核参与，把它单独拿出来就能直接测。

**改法**
改前（`init.cpp`）：
```cpp
    std::string_view words[MAX_AUTORUN_WORDS];
    u32 count = 0;
    while (!command.empty()) {
        ...
    }
    if (count == 0) {
        return Status::InvArgs;
    }
    ...
    return ou::process_spawn_boot(job, to_str(words[0]), bootstrap_there);
```
改后：
```cpp
    init::AutorunCommand parsed;
    if (auto const status = init::AutorunCommand::parse(command, &parsed); status != Status::Ok) {
        return status;
    }
    ...
    return ou::process_spawn_boot(job, to_str(parsed.name()), bootstrap_there);
```
`AutorunCommand` 的写法照 `runtime::ProcessArgs::parse()`：一个静态的 `parse(input, out) -> Status`，只有返回 Ok 时才写 `|out|`，读取通过 `name()`、`words()` 访问器。全部是 `constexpr`，所以测试里还加了一条 `static_assert`。

构建上遇到两个坑，记下来：
- 不能在 init 旁边放一个 `group("tests")` 再从 `//:default` 引用它：那样 init 的 `BUILD.gn` 会在默认工具链里被加载，而文件开头断言了必须是 user 工具链。所以改为在 `//:default` 里直接写出 `test:autorun($host_toolchain)`。
- host 工具链会加载 init 的 `BUILD.gn`（为了 `:autorun`），GN 会定义这个文件里的每一个目标，而 `user_executable` 在 host 下配置不起来（`configs -= "//system:user.executable"` 找不到要删的项）。所以把它包进 `if (!is_host_toolchain)`，和 `//system/lib/runtime` 的做法一样。

**参照**
- `Fuchsia console-launcher main.cc` 的 `LaunchAutorun()`：`fxl::SplitStringCopy(args, "+", kTrimWhitespace, kSplitWantNonEmpty)`。这里跳过空词和它一致；它还会去掉每个词首尾的空白，这里不做，因为 cmdline 的一个词里本来就不会有空白。

**验证**
- 主机单测 `autorun` 5 个全部通过：只有名字；`+` 分隔出 argv[1]、argv[2]；首尾和中间的空词被跳过（`+fault_probe++none+extra+` → `fault_probe none extra`）；空串和 `+++` 返回 InvArgs；8 个词可以，9 个词返回 OutOfRange 且不改动 `out`。
- clang-tidy 在 `init.cpp` 和测试文件上没有诊断。
- QEMU dev（cmdline 临时加 `ours.autorun.boot=+fault_probe++none+`）：`fault_probe: none`、`unknown probe none`，shell 照常回答 pong。
- QEMU dbg_1：shell 的各组检查全部通过。

**故意没做**
- 用一次开机来验证整条 argv：单测已经覆盖了拆分，开机只需要确认拆分结果真的到了程序手里（argv[1] 就够了）。
- 去掉每个词首尾的空白：理由见「参照」。
