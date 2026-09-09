# 调试内核

## 一键

用 `ours.code-workspace` 打开这个仓库（`File > Open Workspace from File...`，或者
在 Finder 里双击它），然后按 **F5**。就这一步。

> 必须从这个文件打开：调试配置和它依赖的那几个任务都写在 `ours.code-workspace`
> 里，直接 `Open Folder` 打开目录的话 VS Code 读不到它们，调试面板会是空的。

它会：编译 → 用 QEMU 启动镜像并**停在第一条指令之前** → 挂上 lldb。会话开始时机器
停在复位向量 `0xfff0`（固件都还没跑），这时候把断点打上，再按继续（F5）——**最早期的
启动代码也能断下来**，这正是要 `-S` 的原因。

结束调试会话时 QEMU 会被一并杀掉。

有三个配置可选（调试面板的下拉框）：

| 配置 | 用途 |
|---|---|
| `Kernel: debug (dbg.1, asserts on)` | 默认。`KERNEL_DEBUG_LEVEL=1`，`DEBUG_ASSERT` 和 trace/debug 日志都在 |
| `Kernel: debug (dev, asserts off)` | 断言和 trace 都被编译掉的那个镜像。**一个 bug 在一个镜像里断言、在另一个里静默**，所以两个都要能调 |
| `Kernel: attach to a QEMU I started myself` | 已经在终端里跑着 `./scripts/qemu-debug.sh` 时用，不重启它 |

不需要任何安装。调试适配器是 `llvm-vs-code-extensions.lldb-dap` 扩展（已装），它经
`xcrun` 找到系统的 `lldb-dap`；QEMU、GRUB、clang 全部来自 `//prebuilt`。

## 为什么是「停在第一条指令之前」

QEMU 用 `-S` 启动。这不是保守，是必要的：等到一个**正在运行**的内核可以被挂上时，
`start_kernel` 早就过去了，早期启动的一切都没法下断点。

代价是会话一开始停在固件之前，要自己按一下继续。**不要**用 `postRunCommands` 里加
`continue` 来省这一下：那串命令跑在 DAP 的 attach 请求里，早于编辑器把断点下发给适配器，
省掉的一下换来的是早期断点被跑过去。

## 为什么是 attach 而不是 launch

`lldb-dap` 的 `launch` 要求 `program`，并且要把它当本地进程拉起来——QEMU 里的内核不是
本地进程。只有 `attach` 提供 `attachCommands`：一串"代替按 pid 附加"执行、并且**允许自己
建 target** 的命令。配置里那三条（`target create` / `target modules load --slide` /
`gdb-remote`）就挂在这个钩子上。

## 命令行

同一套东西不进 VS Code 也能用：

```bash
./scripts/qemu-debug.sh
```

它清掉上一次会话残留的 QEMU/lldb（**两个进程抢同一个端口会互相打死**，而活下来的不一定
是新的那个），启动 QEMU，**轮询到端口真的接受连接**才打印 ready ——不是 sleep 一个猜的
秒数：QEMU 被 exec 之前还要重建 ESP（grub-mkimage、mformat、mcopy）。

另开一个终端：

```bash
lldb -o 'target create out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours' \
     -o 'target modules load --file vmours --slide 0xffffff8840000000' \
     -o 'gdb-remote localhost:1234'
```

换镜像：`IMAGE=$PWD/out/bringup.x86/kernel.dev.obi ./scripts/qemu-debug.sh`
（符号目录相应换成 `kernel_x86.dbg.0`）。换端口：`PORT=1235`。

## 那个 slide 是什么

内核链接在地址 0，而 physboot 把它加载到 `KERNEL_LOAD_BASE`。不滑动符号的话每个地址都差
64 GiB，断点会打在虚空里。

`0xffffff8840000000` = `KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE + GB(1)`
（见 `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp`）。

**它不是永远不变的**：一旦启用 KASLR，镜像会带一个最多 GB(1) 的偏移。真实值在启动日志里：

```
Reserve area k:head: [FFFFFF8840000000, FFFFFF8840007000)
```

左边那个数就是 slide。

## 不用调试器往往更快

内核不退出，所以看日志是：

```bash
( IMAGE=$PWD/out/bringup.x86/kernel.dev.dbg_1.obi ./scripts/qemu-boot.sh > /tmp/boot.log 2>&1 & \
  pid=$!; sleep 30; kill $pid 2>/dev/null; wait $pid 2>/dev/null )
grep -a "..." /tmp/boot.log
```

两点值得记住：

- **`grep` 要带 `-a`**。启动日志里有终端转义和 NUL 字节，`grep` 会把它当二进制文件**静默
  跳过**——你会以为什么都没匹配上。
- **日志末尾如果没有 `terminating on signal 15`，说明 QEMU 是自己退的**，也就是三重故障，
  不是挂死。

追一处静默停止时，临时插几行标了 `// PROBE` 的 `log::trace` 再删掉，常常比 gdbstub 快
——gdbstub 下的启动要慢上不少。
